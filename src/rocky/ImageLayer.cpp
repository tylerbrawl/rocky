/**
 * rocky c++
 * Copyright 2023-2024 Pelican Mapping, Chris Djali
 * MIT License
 */
#pragma once
#include "ImageLayer.h"

#include "Color.h"
#include "IOTypes.h"
#include "Metrics.h"
#include "Utils.h"
#include "GeoImage.h"
#include "Image.h"
#include "TileKey.h"
#include "json.h"

#include <cinttypes>

using namespace ROCKY_NAMESPACE;
using namespace ROCKY_NAMESPACE::util;

#define LC "[ImageLayer] \"" << name().value() << "\" "

namespace
{
    class CompositeImage : public Inherit<Image, CompositeImage>
    {
    public:
        CompositeImage(
            PixelFormat format,
            unsigned s,
            unsigned t,
            unsigned r = 1) :
            super(format, s, t, r),
            dependencies(),
            cleanupOperation()
        {}

        CompositeImage(const CompositeImage& rhs) :
            super(rhs),
            dependencies(rhs.dependencies)
        {}

        virtual ~CompositeImage()
        {
            cleanupOperation();
        }

        std::vector<std::shared_ptr<Image>> dependencies;
        std::function<void()> cleanupOperation;
    };
}

ImageLayer::ImageLayer() :
    super()
{
    construct({});
}

ImageLayer::ImageLayer(const JSON& conf) :
    super(conf)
{
    construct(conf);
}

void
ImageLayer::construct(const JSON& conf)
{
    const auto j = parse_json(conf);
    get_to(j, "nodata_image", _noDataImageLocation);
    get_to(j, "transparent_color", _transparentColor);
    get_to(j, "texture_compression", _textureCompression);

    setRenderType(RENDERTYPE_TERRAIN_SURFACE);

    _dependencyCache = std::make_shared<DependencyCache<TileKey, Image>>();
}

JSON
ImageLayer::to_json() const
{
    auto j = parse_json(super::to_json());
    set(j, "nodata_image", _noDataImageLocation);
    set(j, "transparent_color", _transparentColor);
    set(j, "texture_compression", _textureCompression);
    return j.dump();
}

Result<GeoImage>
ImageLayer::createImage(const TileKey& key) const
{
    return createImage(key, IOOptions());
}

Result<GeoImage>
ImageLayer::createImage(const TileKey& key, const IOOptions& io) const
{    
    ROCKY_PROFILING_ZONE;
    ROCKY_PROFILING_ZONE_TEXT(getName() + " " + key.str());

    if (!isOpen())
    {
        return Result(GeoImage::INVALID);
    }

    //NetworkMonitor::ScopedRequestLayer layerRequest(getName());

    auto result = createImageInKeyProfile(key, io);

#if 0
    // Post-cache operations:
    if (!_postLayers.empty())
    {
        for (auto& post : _postLayers)
        {
            if (!result.valid())
            {
                TileKey bestKey = bestAvailableTileKey(key);
                result = createImageInKeyProfile(bestKey, progress);
            }

            result = post->createImage(result, key, progress);
        }
    }

    if (result.valid())
    {
        postCreateImageImplementation(result, key, progress);
    }
#endif

    return result;
}

Result<GeoImage>
ImageLayer::createImage(const GeoImage& canvas, const TileKey& key, const IOOptions& io)
{
    std::shared_lock lock(layerStateMutex());
    return createImageImplementation(canvas, key, io);
}

Result<GeoImage>
ImageLayer::createImageInKeyProfile(const TileKey& key, const IOOptions& io) const
{
    // If the layer is disabled, bail out.
    if ( !isOpen() )
    {
        return Result(GeoImage::INVALID);
    }

    // Make sure the request is in range.
    // TODO: perhaps this should be a call to mayHaveData(key) instead.
    if ( !isKeyInLegalRange(key) )
    {
        return Result(GeoImage::INVALID);
    }

    // Tile gate prevents two threads from requesting the same key
    // at the same time, which would be unnecessary work. Only lock
    // the gate if there is an L2 cache active
    ROCKY_TODO("Sentry");
    //util::ScopedGate<TileKey> scopedGate(_sentry, key, [&]() {
    //    return _memCache.valid();
    //});

    Result<GeoImage> result;

    // if this layer has no profile, just go straight to the driver.
    if (!profile().valid())
    {
        std::shared_lock lock(layerStateMutex());
        return createImageImplementation(key, io);
    }

    else if (key.profile() == profile())
    {
        std::shared_lock lock(layerStateMutex());
        result = createImageImplementation(key, io);
    }
    else
    {
        // If the profiles are different, use a compositing method to assemble the tile.
        auto image = assembleImage(key, io);
        result = GeoImage(image, key.extent());
    }

    return result;
}

shared_ptr<Image>
ImageLayer::assembleImage(const TileKey& key, const IOOptions& io) const
{
    shared_ptr<CompositeImage> output;

    // Collect the rasters for each of the intersecting tiles.
    std::vector<GeoImage> source_list;

    // Determine the intersecting keys
    std::vector<TileKey> intersectingKeys;

    // TODO: This will ensure that enough data is availble to fill any gaps in the tile
    // at the requested LOD by "falling back" to lower LODs. This is a temporary solution
    // thought b/c although it works, it will end up creating a lot of extra image tiles
    // that don't get used. A better approach will be to request data on demand as we go
    // or to maange a spatial index of sources to ensure full coverage. -gw 6/17/24
    if (key.levelOfDetail() > 0u)
    {
        TileKey currentKey = key;
        while (currentKey.levelOfDetail() > 0u)
        {
            std::vector<TileKey> intermediate;
            currentKey.getIntersectingKeys(profile(), intermediate);
            intersectingKeys.insert(intersectingKeys.end(), intermediate.begin(), intermediate.end());
            currentKey.makeParent();
        }
    }

    else
    {
        // LOD is zero - check whether the LOD mapping went out of range, and if so,
        // fall back until we get valid tiles. This can happen when you have two
        // profiles with very different tile schemes, and the "equivalent LOD"
        // surpasses the max data LOD of the tile source.
        unsigned numTilesThatMayHaveData = 0u;

        int intersectionLOD = profile().getEquivalentLOD(key.profile(), key.levelOfDetail());

        while (numTilesThatMayHaveData == 0u && intersectionLOD >= 0)
        {
            intersectingKeys.clear();

            TileKey::getIntersectingKeys(
                key.extent(),
                intersectionLOD,
                profile(),
                intersectingKeys);

            for (auto& layerKey : intersectingKeys)
            {
                if (mayHaveData(layerKey))
                {
                    ++numTilesThatMayHaveData;
                }
            }

            --intersectionLOD;
        }
    }

    // collect raster for each intersecting key
    if (intersectingKeys.size() > 0)
    {
        for (auto& layerKey : intersectingKeys)
        {
            if (isKeyInLegalRange(layerKey))
            {
                auto cached = (*_dependencyCache)[layerKey];
                if (cached)
                    source_list.emplace_back(cached, layerKey.extent());
                else
                {
                    std::shared_lock L(layerStateMutex());
                    auto result = createImageImplementation(layerKey, io);

                    if (result.status.ok() && result.value.valid())
                    {
                        cached = _dependencyCache->put(layerKey, result.value.image());
                        source_list.push_back(result.value);
                    }
                }
            }
        }

        // If we actually got data, resample/reproject it to match the incoming TileKey's extents.
        if (source_list.size() > 0)
        {
            unsigned width = 0;
            unsigned height = 0;
            auto keyExtent = key.extent();

            for (auto& source : source_list)
            {
                width = std::max(width, source.image()->width());
                height = std::max(height, source.image()->height());
            }

            // assume all tiles to mosaic are in the same SRS.
            SRSOperation xform = keyExtent.srs().to(source_list[0].srs());

            // new output:
            output = CompositeImage::create(Image::R8G8B8A8_UNORM, width, height);
            output->dependencies.reserve(source_list.size());
            for (auto& source : source_list)
                output->dependencies.push_back(source.image());
            output->cleanupOperation = [captured{ std::weak_ptr(_dependencyCache) }]() {
                auto cache = captured.lock();
                if (cache)
                    cache->clean();
                };

            // working set of points. it's much faster to xform an entire vector all at once.
            std::vector<glm::dvec3> points;
            points.assign(width * height, { 0, 0, 0 });

            double minx, miny, maxx, maxy;
            key.extent().getBounds(minx, miny, maxx, maxy);
            double dx = (maxx - minx) / (double)(width - 1);
            double dy = (maxy - miny) / (double)(height - 1);

            // build a grid of sample points:
            for (unsigned r = 0; r < height; ++r)
            {
                double y = miny + (dy * (double)r);
                for (unsigned c = 0; c < width; ++c)
                {
                    double x = minx + (dx * (double)c);
                    points[r * width + c].x = x;
                    points[r * width + c].y = y;
                }
            }

            // transform the sample points to the SRS of our source data tiles:
            if (xform.valid())
                xform.transformArray(&points[0], points.size());

            //Create the new heightfield by sampling all of them.
            for (unsigned r = 0; r < height; ++r)
            {
                double y = miny + (dy * (double)r);

                for (unsigned int c = 0; c < width; ++c)
                {
                    double x = minx + (dx * (double)c);

                    unsigned i = r * width + c;

                    // For each sample point, try each heightfield.  The first one with a valid elevation wins.
                    glm::fvec4 pixel(0, 0, 0, 0);

                    for (unsigned k = 0; k < source_list.size(); ++k)
                    {
                        // get the elevation value, at the same time transforming it vertically into the
                        // requesting key's vertical datum.
                        if (source_list[k].read(pixel, points[i].x, points[i].y) && 
                            pixel.a > 0.0f)
                        {
                            break;
                        }
                    }

                    output->write(pixel, c, r);
                }
            }
        }
    }

    // If the progress was cancelled clear out any of the output data.
    if (io.canceled())
    {
        output = nullptr;
    }

    return output;
}

Status
ImageLayer::writeImage(const TileKey& key, shared_ptr<Image> image, const IOOptions& io)
{
    if (status().failed())
        return status();

    std::shared_lock lock(layerStateMutex());
    return writeImageImplementation(key, image, io);
}

Status
ImageLayer::writeImageImplementation(const TileKey& key, shared_ptr<Image> image, const IOOptions& io) const
{
    return Status(Status::ServiceUnavailable);
}

const std::string
ImageLayer::getCompressionMethod() const
{
    return _textureCompression;
}

void
ImageLayer::modifyTileBoundingBox(const TileKey& key, Box& box) const
{
    //if (_altitude.has_value())
    //{
    //    if (_altitude->as(Units::METERS) > box.zmax)
    //    {
    //        box.zmax = _altitude->as(Units::METERS);
    //    }
    //}
    super::modifyTileBoundingBox(key, box);
}
