/**
 * rocky c++
 * Copyright 2023 Pelican Mapping
 * MIT License
 */
#include "GDALLayers.h"
#include "Heightfield.h"
#include "Image.h"

#include <gdal.h>
#include <gdalwarper.h>
#include <ogr_spatialref.h>

#include <filesystem>

using namespace rocky;
using namespace rocky::GDAL;

#undef LC
#define LC "[GDAL] \"" << getName() << "\" "

#define INDENT ""

#if (GDAL_VERSION_MAJOR > 1 || (GDAL_VERSION_MAJOR >= 1 && GDAL_VERSION_MINOR >= 5))
#  define GDAL_VERSION_1_5_OR_NEWER 1
#endif

#if (GDAL_VERSION_MAJOR > 1 || (GDAL_VERSION_MAJOR >= 1 && GDAL_VERSION_MINOR >= 6))
#  define GDAL_VERSION_1_6_OR_NEWER 1
#endif

#ifndef GDAL_VERSION_1_5_OR_NEWER
#  error "**** GDAL 1.5 or newer required ****"
#endif

//GDAL proxy is only available after GDAL 1.6
#if GDAL_VERSION_1_6_OR_NEWER
#  include <gdal_proxy.h>
#endif

#if (GDAL_VERSION_MAJOR >= 2)
#  define GDAL_VERSION_2_0_OR_NEWER 1
#endif

#include <cpl_string.h>

//GDAL VRT api is only available after 1.5.0
#include <gdal_vrt.h>

#define GEOTRSFRM_TOPLEFT_X            0
#define GEOTRSFRM_WE_RES               1
#define GEOTRSFRM_ROTATION_PARAM1      2
#define GEOTRSFRM_TOPLEFT_Y            3
#define GEOTRSFRM_ROTATION_PARAM2      4
#define GEOTRSFRM_NS_RES               5


namespace rocky
{
    namespace GDAL
    {
        // From easyrgb.com
        float Hue_2_RGB(float v1, float v2, float vH)
        {
            if (vH < 0.0f) vH += 1.0f;
            if (vH > 1.0f) vH -= 1.0f;
            if ((6.0f * vH) < 1.0f) return (v1 + (v2 - v1) * 6.0f * vH);
            if ((2.0f * vH) < 1.0f) return (v2);
            if ((3.0f * vH) < 2.0f) return (v1 + (v2 - v1) * ((2.0f / 3.0f) - vH) * 6.0f);
            return (v1);
        }

#ifndef GDAL_VERSION_2_0_OR_NEWER
        // RasterIO was substantially improved in 2.0
        // See https://trac.osgeo.org/gdal/wiki/rfc51_rasterio_resampling_progress
        typedef int GSpacing;
#endif

        typedef enum
        {
            LOWEST_RESOLUTION,
            HIGHEST_RESOLUTION,
            AVERAGE_RESOLUTION
        } ResolutionStrategy;

        typedef struct
        {
            int    isFileOK;
            int    nRasterXSize;
            int    nRasterYSize;
            double adfGeoTransform[6];
            int    nBlockXSize;
            int    nBlockYSize;
        } DatasetProperty;

        typedef struct
        {
            GDALColorInterp        colorInterpretation;
            GDALDataType           dataType;
            GDALColorTableH        colorTable;
            int                    bHasNoData;
            double                 noDataValue;
        } BandProperty;


        // This is simply the method GDALAutoCreateWarpedVRT() with the GDALSuggestedWarpOutput
        // logic replaced with something that will work properly for polar projections.
        // see: http://www.mail-archive.com/gdal-dev@lists.osgeo.org/msg01491.html
        GDALDatasetH GDALAutoCreateWarpedVRTforPolarStereographic(
            GDALDatasetH hSrcDS,
            const char *pszSrcWKT,
            const char *pszDstWKT,
            GDALResampleAlg eResampleAlg,
            double dfMaxError,
            const GDALWarpOptions *psOptionsIn)
        {
            GDALWarpOptions *psWO;
            int i;

            VALIDATE_POINTER1(hSrcDS, "GDALAutoCreateWarpedVRTForPolarStereographic", NULL);

            /* -------------------------------------------------------------------- */
            /*      Populate the warp options.                                      */
            /* -------------------------------------------------------------------- */
            if (psOptionsIn != NULL)
                psWO = GDALCloneWarpOptions(psOptionsIn);
            else
                psWO = GDALCreateWarpOptions();

            psWO->eResampleAlg = eResampleAlg;

            psWO->hSrcDS = hSrcDS;

            psWO->nBandCount = GDALGetRasterCount(hSrcDS);
            psWO->panSrcBands = (int *)CPLMalloc(sizeof(int) * psWO->nBandCount);
            psWO->panDstBands = (int *)CPLMalloc(sizeof(int) * psWO->nBandCount);

            for (i = 0; i < psWO->nBandCount; i++)
            {
                psWO->panSrcBands[i] = i + 1;
                psWO->panDstBands[i] = i + 1;
            }

            /* TODO: should fill in no data where available */

            /* -------------------------------------------------------------------- */
            /*      Create the transformer.                                         */
            /* -------------------------------------------------------------------- */
            psWO->pfnTransformer = GDALGenImgProjTransform;
            psWO->pTransformerArg =
                GDALCreateGenImgProjTransformer(psWO->hSrcDS, pszSrcWKT,
                    NULL, pszDstWKT,
                    TRUE, 1.0, 0);

            if (psWO->pTransformerArg == NULL)
            {
                GDALDestroyWarpOptions(psWO);
                return NULL;
            }

            /* -------------------------------------------------------------------- */
            /*      Figure out the desired output bounds and resolution.            */
            /* -------------------------------------------------------------------- */
            double adfDstGeoTransform[6];
            int    nDstPixels, nDstLines;
            CPLErr eErr;

            eErr =
                GDALSuggestedWarpOutput(hSrcDS, psWO->pfnTransformer,
                    psWO->pTransformerArg,
                    adfDstGeoTransform, &nDstPixels, &nDstLines);

            // override the suggestions:
            nDstPixels = GDALGetRasterXSize(hSrcDS) * 4;
            nDstLines = GDALGetRasterYSize(hSrcDS) / 2;
            adfDstGeoTransform[0] = -180.0;
            adfDstGeoTransform[1] = 360.0 / (double)nDstPixels;
            //adfDstGeoTransform[2] = 0.0;
            //adfDstGeoTransform[4] = 0.0;
            //adfDstGeoTransform[5] = (-90 -adfDstGeoTransform[3])/(double)nDstLines;

            /* -------------------------------------------------------------------- */
            /*      Update the transformer to include an output geotransform        */
            /*      back to pixel/line coordinates.                                 */
            /*                                                                      */
            /* -------------------------------------------------------------------- */
            GDALSetGenImgProjTransformerDstGeoTransform(
                psWO->pTransformerArg, adfDstGeoTransform);

            /* -------------------------------------------------------------------- */
            /*      Do we want to apply an approximating transformation?            */
            /* -------------------------------------------------------------------- */
            if (dfMaxError > 0.0)
            {
                psWO->pTransformerArg =
                    GDALCreateApproxTransformer(psWO->pfnTransformer,
                        psWO->pTransformerArg,
                        dfMaxError);
                psWO->pfnTransformer = GDALApproxTransform;
            }

            /* -------------------------------------------------------------------- */
            /*      Create the VRT file.                                            */
            /* -------------------------------------------------------------------- */
            GDALDatasetH hDstDS;

            hDstDS = GDALCreateWarpedVRT(hSrcDS, nDstPixels, nDstLines,
                adfDstGeoTransform, psWO);

            GDALDestroyWarpOptions(psWO);

            if (pszDstWKT != NULL)
                GDALSetProjection(hDstDS, pszDstWKT);
            else if (pszSrcWKT != NULL)
                GDALSetProjection(hDstDS, pszDstWKT);
            else if (GDALGetGCPCount(hSrcDS) > 0)
                GDALSetProjection(hDstDS, GDALGetGCPProjection(hSrcDS));
            else
                GDALSetProjection(hDstDS, GDALGetProjectionRef(hSrcDS));

            return hDstDS;
        }

        /**
         * Gets the GeoExtent of the given filename.
         */
        GeoExtent getGeoExtent(std::string& filename)
        {
            GDALDataset* ds = (GDALDataset*)GDALOpen(filename.c_str(), GA_ReadOnly);
            if (!ds)
            {
                return GeoExtent::INVALID;
            }

            // Get the geotransforms
            double geotransform[6];
            ds->GetGeoTransform(geotransform);

            double minX, minY, maxX, maxY;

            GDALApplyGeoTransform(geotransform, 0.0, ds->GetRasterYSize(), &minX, &minY);
            GDALApplyGeoTransform(geotransform, ds->GetRasterXSize(), 0.0, &maxX, &maxY);

            std::string srsString = ds->GetProjectionRef();
            auto srs = SRS::get(srsString);

            GDALClose(ds);

            GeoExtent ext(srs, minX, minY, maxX, maxY);
            return ext;
        }
        /**
        * Finds a raster band based on color interpretation
        */
        GDALRasterBand* findBandByColorInterp(GDALDataset *ds, GDALColorInterp colorInterp)
        {
            for (int i = 1; i <= ds->GetRasterCount(); ++i)
            {
                if (ds->GetRasterBand(i)->GetColorInterpretation() == colorInterp) return ds->GetRasterBand(i);
            }
            return 0;
        }

        GDALRasterBand* findBandByDataType(GDALDataset *ds, GDALDataType dataType)
        {
            for (int i = 1; i <= ds->GetRasterCount(); ++i)
            {
                if (ds->GetRasterBand(i)->GetRasterDataType() == dataType) return ds->GetRasterBand(i);
            }
            return 0;
        }

        bool getPalleteIndexColor(GDALRasterBand* band, int index, u8vec4& color)
        {
            const GDALColorEntry *colorEntry = band->GetColorTable()->GetColorEntry(index);
            GDALPaletteInterp interp = band->GetColorTable()->GetPaletteInterpretation();
            if (!colorEntry)
            {
                //FIXME: What to do here?

                //ROCKY_INFO << "NO COLOR ENTRY FOR COLOR " << rawImageData[i] << std::endl;
                color.r = 255;
                color.g = 0;
                color.b = 0;
                color.a = 1;
                return false;
            }
            else
            {
                if (interp == GPI_RGB)
                {
                    color.r = colorEntry->c1;
                    color.g = colorEntry->c2;
                    color.b = colorEntry->c3;
                    color.a = colorEntry->c4;
                }
                else if (interp == GPI_CMYK)
                {
                    // from wikipedia.org
                    short C = colorEntry->c1;
                    short M = colorEntry->c2;
                    short Y = colorEntry->c3;
                    short K = colorEntry->c4;
                    color.r = 255 - C * (255 - K) - K;
                    color.g = 255 - M * (255 - K) - K;
                    color.b = 255 - Y * (255 - K) - K;
                    color.a = 255;
                }
                else if (interp == GPI_HLS)
                {
                    // from easyrgb.com
                    float H = colorEntry->c1;
                    float S = colorEntry->c3;
                    float L = colorEntry->c2;
                    float R, G, B;
                    if (S == 0)                       //HSL values = 0 - 1
                    {
                        R = L;                      //RGB results = 0 - 1
                        G = L;
                        B = L;
                    }
                    else
                    {
                        float var_2, var_1;
                        if (L < 0.5)
                            var_2 = L * (1 + S);
                        else
                            var_2 = (L + S) - (S * L);

                        var_1 = 2 * L - var_2;

                        R = Hue_2_RGB(var_1, var_2, H + (1.0f / 3.0f));
                        G = Hue_2_RGB(var_1, var_2, H);
                        B = Hue_2_RGB(var_1, var_2, H - (1.0f / 3.0f));
                    }
                    color.r = static_cast<unsigned char>(R*255.0f);
                    color.g = static_cast<unsigned char>(G*255.0f);
                    color.b = static_cast<unsigned char>(B*255.0f);
                    color.a = static_cast<unsigned char>(255.0f);
                }
                else if (interp == GPI_Gray)
                {
                    color.r = static_cast<unsigned char>(colorEntry->c1*255.0f);
                    color.g = static_cast<unsigned char>(colorEntry->c1*255.0f);
                    color.b = static_cast<unsigned char>(colorEntry->c1*255.0f);
                    color.a = static_cast<unsigned char>(255.0f);
                }
                else
                {
                    return false;
                }
                return true;
            }
        }
        
        template<typename T>
        void applyScaleAndOffset(void* data, int count, double scale, double offset)
        {
            T* f = (T*)data;
            for (int i = 0; i < count; ++i)
            {
                double value = static_cast<double>(*f) * scale + offset;
                *f++ = static_cast<T>(value);
            }
        }

        // GDALRasterBand::RasterIO helper method
        bool rasterIO(
            GDALRasterBand *band,
            GDALRWFlag eRWFlag,
            int nXOff,
            int nYOff,
            int nXSize,
            int nYSize,
            void *pData,
            int nBufXSize,
            int nBufYSize,
            GDALDataType eBufType,
            GSpacing nPixelSpace,
            GSpacing nLineSpace,
            Image::Interpolation interpolation = Image::NEAREST
        )
        {
#if GDAL_VERSION_2_0_OR_NEWER
            GDALRasterIOExtraArg psExtraArg;

            // defaults to GRIORA_NearestNeighbour
            INIT_RASTERIO_EXTRA_ARG(psExtraArg);

            switch (interpolation)
            {
            case Image::AVERAGE:
                //psExtraArg.eResampleAlg = GRIORA_Average;
                // for some reason gdal's average resampling produces artifacts occasionally for imagery at higher levels.
                // for now we'll just use bilinear interpolation under the hood until we can understand what is going on.
                psExtraArg.eResampleAlg = GRIORA_Bilinear;
                break;
            case Image::BILINEAR:
                psExtraArg.eResampleAlg = GRIORA_Bilinear;
                break;
            case Image::CUBIC:
                psExtraArg.eResampleAlg = GRIORA_Cubic;
                break;
            case Image::CUBICSPLINE:
                psExtraArg.eResampleAlg = GRIORA_CubicSpline;
                break;
            }

            CPLErr err = band->RasterIO(eRWFlag, nXOff, nYOff, nXSize, nYSize, pData, nBufXSize, nBufYSize, eBufType, nPixelSpace, nLineSpace, &psExtraArg);
#else
            if (interpolation != Image::NEAREST)
            {
                ROCKY_DEBUG << "RasterIO falling back to NEAREST.\n";
            }
            CPLErr err = band->RasterIO(eRWFlag, nXOff, nYOff, nXSize, nYSize, pData, nBufXSize, nBufYSize, eBufType, nPixelSpace, nLineSpace);
#endif
            if (err != CE_None)
            {
                //ROCKY_WARN << LC << "RasterIO failed.\n";
            }
            else
            {
                double scale = band->GetScale();
                double offset = band->GetOffset();

                if (scale != 1.0 || offset != 0.0)
                {
                    int count = nBufXSize * nBufYSize;

                    if (eBufType == GDT_Float32)
                        applyScaleAndOffset<float>(pData, count, scale, offset);
                    else if (eBufType == GDT_Float64)
                        applyScaleAndOffset<double>(pData, count, scale, offset);
                    else if (eBufType == GDT_Int16)
                        applyScaleAndOffset<short>(pData, count, scale, offset);
                    else if (eBufType == GDT_Int32)
                        applyScaleAndOffset<int>(pData, count, scale, offset);
                    else if (eBufType == GDT_Byte)
                        applyScaleAndOffset<char>(pData, count, scale, offset);
                }
            }

            return (err == CE_None);
        }
    }
} // namespace rocky::GDAL

//...................................................................

GDAL::Driver::Driver() :
    _srcDS(NULL),
    _warpedDS(NULL),
    _maxDataLevel(30),
    _linearUnits(1.0)
{
    _threadId = rocky::util::getCurrentThreadId();
}

GDAL::Driver::~Driver()
{
    if (_warpedDS)
        GDALClose(_warpedDS);
    else if (_srcDS)
        GDALClose(_srcDS);

    ROCKY_DEBUG << "Closed GDAL Driver on thread " << _threadId << std::endl;
}

// Open the data source and prepare it for reading
Status
GDAL::Driver::open(
    const std::string& name,
    const GDAL::Options& options,
    unsigned tileSize,
    DataExtentList* layerDataExtents,
    const IOOptions& io)
{
    bool info = (layerDataExtents != NULL);

    _name = name;
    _gdalOptions = options;

    // Is a valid external GDAL dataset specified ?
    bool useExternalDataset = false;
    if (_externalDataset && _externalDataset->dataset() != NULL)
    {
        useExternalDataset = true;
    }

    if (useExternalDataset == false &&
        (!gdalOptions().url.isSet() || gdalOptions().url->empty()) &&
        (!gdalOptions().connection.isSet() || gdalOptions().connection->empty()))
    {
        return Status::Error(Status::ConfigurationError, "No URL, directory, or connection string specified");
    }

    // source connection:
    std::string source;
    bool isFile = true;

    if (gdalOptions().url.isSet())
    {
        // Use the base instead of the full if this is a gdal virtual file system
        if (util::startsWith(gdalOptions().url->base(), "/vsi"))
        {
            source = gdalOptions().url->base();
        }
        else
        {
            source = gdalOptions().url->full();
        }
    }
    else if (gdalOptions().connection.isSet())
    {
        source = gdalOptions().connection.get();
        isFile = false;
    }

    if (useExternalDataset == false)
    {
        std::string input;

        if (gdalOptions().url.isSet())
            input = gdalOptions().url->full();
        else
            input = source;

        if (input.empty())
        {
            return Status::Error(Status::ResourceUnavailable, "Could not find any valid input.");
        }

        // Resolve the pathname...
        if (isFile && !std::filesystem::exists(input))
        {
            // TODO?
            //std::string found = osgDB::findDataFile(input);
            //if (!found.empty())
            //    input = found;
        }

        // Create the source dataset:
        _srcDS = (GDALDataset*)GDALOpen(input.c_str(), GA_ReadOnly);
        if (_srcDS)
        {
            char **subDatasets = _srcDS->GetMetadata("SUBDATASETS");
            int numSubDatasets = CSLCount(subDatasets);

            if (numSubDatasets > 0)
            {
                int subDataset = gdalOptions().subDataSet.isSet() ? *gdalOptions().subDataSet : 1;
                if (subDataset < 1 || subDataset > numSubDatasets) subDataset = 1;
                std::stringstream buf;
                buf << "SUBDATASET_" << subDataset << "_NAME";
                char *pszSubdatasetName = CPLStrdup(CSLFetchNameValue(subDatasets, buf.str().c_str()));
                GDALClose(_srcDS);
                _srcDS = (GDALDataset*)GDALOpen(pszSubdatasetName, GA_ReadOnly);
                CPLFree(pszSubdatasetName);
            }
        }

        if (!_srcDS)
        {
            return Status::Error(Status::ResourceUnavailable, "Failed to open " + input);
        }
    }
    else
    {
        _srcDS = _externalDataset->dataset();
    }

    // Establish the source spatial reference:
    shared_ptr<SRS> src_srs;

    std::string srcProj = _srcDS->GetProjectionRef();

    // If the projection is empty and we have GCP's then use the GCP projection.
    if (srcProj.empty() && _srcDS->GetGCPCount() > 0)
    {
        srcProj = _srcDS->GetGCPProjection();
    }

    if (!srcProj.empty())
    {
        src_srs = SRS::get(srcProj);
    }

    // still no luck? (for example, an ungeoreferenced file lika jpeg?)
    // try to read a .prj file:
    if (!src_srs)
    {
        // not found in the dataset; try loading a .prj file
        //std::string prjLocation = osgDB::getNameLessExtension(source) + std::string(".prj");
        std::string prjLocation = util::Path(source).replace_extension("prj").string();

        auto rr = URI(prjLocation).read(nullptr); // TODO io
        if (rr.status.ok() && rr.value.data.valid())
        {

        //ReadResult r = URI(prjLocation).readString(readOptions);
        //if (r.succeeded())
        //{
            src_srs = SRS::get(util::trim(rr.value.data.to_string()));
        }
    }

    if (!src_srs)
    {
        return Status::Error(Status::ResourceUnavailable,
            "Dataset has no spatial reference information (" + source + ")");
    }

    // These are the actual extents of the data:
    bool hasGCP = false;
    bool isRotated = false;
    bool requiresReprojection = false;
    
    bool hasGeoTransform = (_srcDS->GetGeoTransform(_geotransform) == CE_None);

    hasGCP = _srcDS->GetGCPCount() > 0 && _srcDS->GetGCPProjection();
    isRotated = hasGeoTransform && (_geotransform[2] != 0.0 || _geotransform[4] != 0.0);
    requiresReprojection = hasGCP || isRotated;

    // For a geographic SRS, use the whole-globe profile for performance.
    // Otherwise, collect information and make the profile later.
    if (src_srs->isGeographic())
    {
        ROCKY_DEBUG << INDENT << "Creating Profile from source's geographic SRS: " << src_srs->getName() << std::endl;
        _profile = Profile::create(src_srs);
        if (!_profile)
        {
            return Status::Error(Status::ResourceUnavailable,
                "Cannot create geographic Profile from dataset's spatial reference information: " + src_srs->getName());
        }

        // no xform an geographic? Match the profile.
        if (!hasGeoTransform)
        {
            _geotransform[0] = _profile->getExtent().xMin();
            _geotransform[1] = _profile->getExtent().width() / (double)_srcDS->GetRasterXSize();
            _geotransform[2] = 0;
            _geotransform[3] = _profile->getExtent().yMax();
            _geotransform[4] = 0;
            _geotransform[5] = -_profile->getExtent().height() / (double)_srcDS->GetRasterYSize();
            hasGeoTransform = true;
        }
    }

    // Handle some special cases.
    std::string warpedSRSWKT;

    if (requiresReprojection || (_profile && !_profile->getSRS()->isEquivalentTo(src_srs.get())))
    {
        if (_profile && _profile->getSRS()->isGeographic() && (src_srs->isNorthPolar() || src_srs->isSouthPolar()))
        {
            _warpedDS = (GDALDataset*)GDALAutoCreateWarpedVRTforPolarStereographic(
                _srcDS,
                src_srs->getWKT().c_str(),
                _profile->getSRS()->getWKT().c_str(),
                GRA_NearestNeighbour,
                5.0,
                nullptr);
        }
        else
        {
            std::string destWKT = _profile ? _profile->getSRS()->getWKT() : src_srs->getWKT();
            _warpedDS = (GDALDataset*)GDALAutoCreateWarpedVRT(
                _srcDS,
                src_srs->getWKT().c_str(),
                destWKT.c_str(),
                GRA_NearestNeighbour,
                5.0,
                0);
        }

        if (_warpedDS)
        {
            warpedSRSWKT = _warpedDS->GetProjectionRef();
            _warpedDS->GetGeoTransform(_geotransform);
        }
    }
    else
    {
        _warpedDS = _srcDS;
        warpedSRSWKT = src_srs->getWKT();

        // re-read the extents from the new DS:
        _warpedDS->GetGeoTransform(_geotransform);
    }

    if (!_warpedDS)
    {
        return Status::Error("Failed to create a final sampling dataset");
    }

    // calcluate the inverse of the geotransform:
    GDALInvGeoTransform(_geotransform, _invtransform);

    double minX, minY, maxX, maxY;
    pixelToGeo(0.0, _warpedDS->GetRasterYSize(), minX, minY);
    pixelToGeo(_warpedDS->GetRasterXSize(), 0.0, maxX, maxY);

    ROCKY_DEBUG << LC << INDENT << "Bounds: " << minX << "," << minY << " .. " << maxX << "," << maxY << std::endl;

    // If we don't have a profile yet, that means this is a projected dataset
    // so we will create the profile from the actual data extents.
    if (!_profile)
    {
        auto srs = SRS::get(warpedSRSWKT);
        if (srs)
        {
            _profile = Profile::create(
                srs,
                Box(minX, minY, maxX, maxY));
        }

        if (!_profile)
        {
            return Status::Error(
                "Cannot create projected Profile from dataset's warped spatial reference WKT: " + warpedSRSWKT);
        }

        if (info)
            ROCKY_INFO << LC << INDENT << source << " is projected, SRS = " << warpedSRSWKT << std::endl;
    }

    ROCKY_HARD_ASSERT(_profile != nullptr);

    //Compute the min and max data levels
    double resolutionX = (maxX - minX) / (double)_warpedDS->GetRasterXSize();
    double resolutionY = (maxY - minY) / (double)_warpedDS->GetRasterYSize();
    double maxResolution = std::min(resolutionX, resolutionY);

    if (info)
        ROCKY_INFO << LC << INDENT << "Resolution= " << resolutionX << "x" << resolutionY << " max=" << maxResolution << std::endl;

    if (_maxDataLevel.isSet())
    {
        if (info)
            ROCKY_INFO << LC << INDENT << gdalOptions().url->full() << " using max data level " << _maxDataLevel.get() << std::endl;
    }
    else
    {
        unsigned int max_level = 30;
        for (unsigned int i = 0; i < max_level; ++i)
        {
            _maxDataLevel = i;
            double w, h;
            _profile->getTileDimensions(i, w, h);
            double resX = w / (double)tileSize;
            double resY = h / (double)tileSize;

            if (resX < maxResolution || resY < maxResolution)
            {
                break;
            }
        }

        if (info)
            ROCKY_INFO << LC << INDENT << gdalOptions().url->full() << " max Data Level: " << _maxDataLevel.get() << std::endl;
    }

    // If the input dataset is a VRT, then get the individual files in the dataset and use THEM for the DataExtents.
    // A VRT will create a potentially very large virtual dataset from sparse datasets, so using the extents from the underlying files
    // will allow rocky to only create tiles where there is actually data.
    DataExtentList dataExtents;

    auto srs = SRS::get(warpedSRSWKT);

    // record the data extent in profile space:
    _bounds = Box(minX, minY, maxX, maxY);

    const char* pora = _srcDS->GetMetadataItem("AREA_OR_POINT");
    bool is_area = pora != nullptr && util::toLower(std::string(pora)) == "area";

    bool clamped = false;
    if (srs->isGeographic())
    {
        if (is_area && (_bounds.xmin < -180.0 || _bounds.xmax > 180.0))
        {
            _bounds.xmin += resolutionX * 0.5;
            _bounds.xmax -= resolutionX * 0.5;
        }

        if ((_bounds.xmax - _bounds.xmin) > 360.0)
        {
            _bounds.xmin = -180;
            _bounds.xmax = 180;
            clamped = true;
        }

        if (is_area && (_bounds.ymin < -90.0 || _bounds.ymax > 90.0))
        {
            _bounds.ymin += resolutionY * 0.5;
            _bounds.ymax -= resolutionY * 0.5;
        }

        if ((_bounds.ymax - _bounds.ymin) > 180)
        {
            _bounds.ymin = -90;
            _bounds.ymax = 90;
            clamped = true;
        }
        if (clamped)
        {
            ROCKY_INFO << LC << "Clamped out-of-range geographic extents" << std::endl;
        }
    }
    _extents = GeoExtent(srs, _bounds);

    ROCKY_DEBUG << LC << "GeoExtent = " << _extents.toString() << std::endl;

    if (layerDataExtents)
    {
        GeoExtent profile_extent = _extents.transform(_profile->getSRS());
        if (dataExtents.empty())
        {
            // Use the extents of the whole file.
            if (_maxDataLevel.isSet())
                layerDataExtents->push_back(DataExtent(profile_extent, 0, _maxDataLevel.get()));
            else
                layerDataExtents->push_back(DataExtent(profile_extent));
        }
        else
        {
            // Use the DataExtents from the subfiles of the VRT.
            layerDataExtents->insert(layerDataExtents->end(), dataExtents.begin(), dataExtents.end());
        }
    }

    // Get the linear units of the SRS for scaling elevation values
    _linearUnits = srs->getReportedLinearUnits();

    if (info)
        ROCKY_DEBUG << LC << INDENT << "Set Profile to " << _profile->toString() << std::endl;

    return STATUS_OK;
}

void
GDAL::Driver::pixelToGeo(double x, double y, double &geoX, double &geoY)
{
    geoX = _geotransform[0] + _geotransform[1] * x + _geotransform[2] * y;
    geoY = _geotransform[3] + _geotransform[4] * x + _geotransform[5] * y;
}

void
GDAL::Driver::geoToPixel(double geoX, double geoY, double &x, double &y)
{
    x = _invtransform[0] + _invtransform[1] * geoX + _invtransform[2] * geoY;
    y = _invtransform[3] + _invtransform[4] * geoX + _invtransform[5] * geoY;

    //Account for slight rounding errors.  If we are right on the edge of the dataset, clamp to the edge
    double eps = 0.0001;
    if (equivalent(x, 0.0, eps)) x = 0;
    if (equivalent(y, 0.0, eps)) y = 0;
    if (equivalent(x, (double)_warpedDS->GetRasterXSize(), eps)) x = _warpedDS->GetRasterXSize();
    if (equivalent(y, (double)_warpedDS->GetRasterYSize(), eps)) y = _warpedDS->GetRasterYSize();

}

bool
GDAL::Driver::isValidValue(float v, GDALRasterBand* band)
{
    float bandNoData = -32767.0f;
    int success;
    float value = band->GetNoDataValue(&success);
    if (success)
    {
        bandNoData = value;
    }

    //Check to see if the value is equal to the bands specified no data
    if (bandNoData == v)
        return false;

    //Check to see if the value is equal to the user specified nodata value
    if (_noDataValue.isSetTo(v))
        return false;

    //Check to see if the user specified a custom min/max
    if (_minValidValue.isSet() && v < _minValidValue.get())
        return false;

    if (_maxValidValue.isSet() && v > _maxValidValue.get())
        return false;

    return true;
}

float
GDAL::Driver::getInterpolatedValue(GDALRasterBand* band, double x, double y, bool applyOffset)
{
    double r, c;
    geoToPixel(x, y, c, r);

    if (applyOffset)
    {
        //Apply half pixel offset
        r -= 0.5;
        c -= 0.5;

        //Account for the half pixel offset in the geotransform.  If the pixel value is -0.5 we are still technically in the dataset
        //since 0,0 is now the center of the pixel.  So, if are within a half pixel above or a half pixel below the dataset just use
        //the edge values
        if (c < 0 && c >= -0.5)
        {
            c = 0;
        }
        else if (c > _warpedDS->GetRasterXSize() - 1 && c <= _warpedDS->GetRasterXSize() - 0.5)
        {
            c = _warpedDS->GetRasterXSize() - 1;
        }

        if (r < 0 && r >= -0.5)
        {
            r = 0;
        }
        else if (r > _warpedDS->GetRasterYSize() - 1 && r <= _warpedDS->GetRasterYSize() - 0.5)
        {
            r = _warpedDS->GetRasterYSize() - 1;
        }
    }

    float result = 0.0f;

    //If the location is outside of the pixel values of the dataset, just return 0
    if (c < 0 || r < 0 || c > _warpedDS->GetRasterXSize() - 1 || r > _warpedDS->GetRasterYSize() - 1)
        return NO_DATA_VALUE;

    if (gdalOptions().interpolation == Image::NEAREST)
    {
        rasterIO(band, GF_Read, (int)round(c), (int)round(r), 1, 1, &result, 1, 1, GDT_Float32, 0, 0);
        if (!isValidValue(result, band))
        {
            return NO_DATA_VALUE;
        }
    }
    else
    {
        int rowMin = std::max((int)floor(r), 0);
        int rowMax = std::max(std::min((int)ceil(r), (int)(_warpedDS->GetRasterYSize() - 1)), 0);
        int colMin = std::max((int)floor(c), 0);
        int colMax = std::max(std::min((int)ceil(c), (int)(_warpedDS->GetRasterXSize() - 1)), 0);

        if (rowMin > rowMax) rowMin = rowMax;
        if (colMin > colMax) colMin = colMax;

        float urHeight, llHeight, ulHeight, lrHeight;

        rasterIO(band, GF_Read, colMin, rowMin, 1, 1, &llHeight, 1, 1, GDT_Float32, 0, 0);
        rasterIO(band, GF_Read, colMin, rowMax, 1, 1, &ulHeight, 1, 1, GDT_Float32, 0, 0);
        rasterIO(band, GF_Read, colMax, rowMin, 1, 1, &lrHeight, 1, 1, GDT_Float32, 0, 0);
        rasterIO(band, GF_Read, colMax, rowMax, 1, 1, &urHeight, 1, 1, GDT_Float32, 0, 0);

        if ((!isValidValue(urHeight, band)) || (!isValidValue(llHeight, band)) || (!isValidValue(ulHeight, band)) || (!isValidValue(lrHeight, band)))
        {
            return NO_DATA_VALUE;
        }

        if (gdalOptions().interpolation == Image::AVERAGE)
        {
            double x_rem = c - (int)c;
            double y_rem = r - (int)r;

            double w00 = (1.0 - y_rem) * (1.0 - x_rem) * (double)llHeight;
            double w01 = (1.0 - y_rem) * x_rem * (double)lrHeight;
            double w10 = y_rem * (1.0 - x_rem) * (double)ulHeight;
            double w11 = y_rem * x_rem * (double)urHeight;

            result = (float)(w00 + w01 + w10 + w11);
        }
        else if (gdalOptions().interpolation == Image::BILINEAR)
        {
            //Check for exact value
            if ((colMax == colMin) && (rowMax == rowMin))
            {
                //ROCKY_NOTICE << "Exact value" << std::endl;
                result = llHeight;
            }
            else if (colMax == colMin)
            {
                //ROCKY_NOTICE << "Vertically" << std::endl;
                //Linear interpolate vertically
                result = ((float)rowMax - r) * llHeight + (r - (float)rowMin) * ulHeight;
            }
            else if (rowMax == rowMin)
            {
                //ROCKY_NOTICE << "Horizontally" << std::endl;
                //Linear interpolate horizontally
                result = ((float)colMax - c) * llHeight + (c - (float)colMin) * lrHeight;
            }
            else
            {
                //ROCKY_NOTICE << "Bilinear" << std::endl;
                //Bilinear interpolate
                float r1 = ((float)colMax - c) * llHeight + (c - (float)colMin) * lrHeight;
                float r2 = ((float)colMax - c) * ulHeight + (c - (float)colMin) * urHeight;

                //ROCKY_INFO << "r1, r2 = " << r1 << " , " << r2 << std::endl;
                result = ((float)rowMax - r) * r1 + (r - (float)rowMin) * r2;
            }
        }
    }

    return result;
}

bool
GDAL::Driver::intersects(const TileKey& key)
{
    return key.getExtent().intersects(_extents);
}

shared_ptr<Image>
GDAL::Driver::createImage(
    const TileKey& key,
    unsigned tileSize,
    bool isCoverage,
    const IOOptions& io)
{
    if (_maxDataLevel.isSet() && key.getLevelOfDetail() > _maxDataLevel.get())
    {
        ROCKY_DEBUG << LC << "Reached maximum data resolution key="
            << key.getLevelOfDetail() << " max=" << _maxDataLevel.get() << std::endl;
        return { };
    }

    if (io.canceled())
    {
        return { };
    }

    //ROCKY_WARN << "key = " << key.str() << std::endl;

    shared_ptr<Image> image;

    const bool invert = true;

    //Get the extents of the tile
    double xmin, ymin, xmax, ymax;
    key.getExtent().getBounds(xmin, ymin, xmax, ymax);

    // Compute the intersection of the incoming key with the data extents of the dataset
    rocky::GeoExtent intersection = key.getExtent().intersectionSameSRS(_extents);
    if (!intersection.valid())
    {
        return { };
    }

    double west = intersection.xMin();
    double east = intersection.xMax();
    double north = intersection.yMax();
    double south = intersection.yMin();

    // The extents and the intersection will be normalized between -180 and 180 longitude if they are geographic.
    // However, the georeferencing will expect the coordinates to be in the same longitude frame as the original dataset,
    // so the intersection bounds are adjusted here if necessary so that the values line up with the georeferencing.
    if (_extents.getSRS()->isGeographic())
    {
        while (west < _bounds.xmin)
        {
            west += 360.0;
            east = west + intersection.width();
        }
        while (west > _bounds.xmax)
        {
            west -= 360.0;
            east = west + intersection.width();
        }
    }

    // Determine the read window
    double src_min_x, src_min_y, src_max_x, src_max_y;
    // Get the pixel coordiantes of the intersection
    geoToPixel(west, intersection.yMax(), src_min_x, src_min_y);
    geoToPixel(east, intersection.yMin(), src_max_x, src_max_y);

    // Convert the doubles to integers.  We floor the mins and ceil the maximums to give the widest window possible.
    src_min_x = floor(src_min_x);
    src_min_y = floor(src_min_y);
    src_max_x = ceil(src_max_x);
    src_max_y = ceil(src_max_y);

    int off_x = (int)(src_min_x);
    int off_y = (int)(src_min_y);
    int width = (int)(src_max_x - src_min_x);
    int height = (int)(src_max_y - src_min_y);


    int rasterWidth = _warpedDS->GetRasterXSize();
    int rasterHeight = _warpedDS->GetRasterYSize();

    // clamp the rasterio bounds so they don't go out of bounds
    if (off_x + width > rasterWidth)
        width = rasterWidth - off_x;

    if (off_y + height > rasterHeight)
        height = rasterHeight - off_x;

    if (off_x + width > rasterWidth || off_y + height > rasterHeight)
    {
        ROCKY_WARN << LC << "Read window outside of bounds of dataset.  Source Dimensions=" << rasterWidth << "x" << rasterHeight << " Read Window=" << off_x << ", " << off_y << " " << width << "x" << height << std::endl;
    }

    // Determine the destination window

    // Compute the offsets in geo coordinates of the intersection from the TileKey
    double offset_left = intersection.xMin() - xmin;
    double offset_top = ymax - intersection.yMax();


    int target_width = (int)ceil((intersection.width() / key.getExtent().width())*(double)tileSize);
    int target_height = (int)ceil((intersection.height() / key.getExtent().height())*(double)tileSize);
    int tile_offset_left = (int)floor((offset_left / key.getExtent().width()) * (double)tileSize);
    int tile_offset_top = (int)floor((offset_top / key.getExtent().height()) * (double)tileSize);

    // Compute spacing
    double dx = (xmax - xmin) / (double)(tileSize - 1);
    double dy = (ymax - ymin) / (double)(tileSize - 1);

    ROCKY_DEBUG << LC << "ReadWindow " << off_x << "," << off_y << " " << width << "x" << height << std::endl;
    ROCKY_DEBUG << LC << "DestWindow " << tile_offset_left << "," << tile_offset_top << " " << target_width << "x" << target_height << std::endl;


    //Return if parameters are out of range.
    if (width <= 0 || height <= 0 || target_width <= 0 || target_height <= 0)
    {
        return { };
    }



    GDALRasterBand* bandRed = findBandByColorInterp(_warpedDS, GCI_RedBand);
    GDALRasterBand* bandGreen = findBandByColorInterp(_warpedDS, GCI_GreenBand);
    GDALRasterBand* bandBlue = findBandByColorInterp(_warpedDS, GCI_BlueBand);
    GDALRasterBand* bandAlpha = findBandByColorInterp(_warpedDS, GCI_AlphaBand);

    GDALRasterBand* bandGray = findBandByColorInterp(_warpedDS, GCI_GrayIndex);

    GDALRasterBand* bandPalette = findBandByColorInterp(_warpedDS, GCI_PaletteIndex);

    if (!bandRed && !bandGreen && !bandBlue && !bandAlpha && !bandGray && !bandPalette)
    {
        ROCKY_DEBUG << LC << "Could not determine bands based on color interpretation, using band count" << std::endl;
        //We couldn't find any valid bands based on the color interp, so just make an educated guess based on the number of bands in the file
        //RGB = 3 bands
        if (_warpedDS->GetRasterCount() == 3)
        {
            bandRed = _warpedDS->GetRasterBand(1);
            bandGreen = _warpedDS->GetRasterBand(2);
            bandBlue = _warpedDS->GetRasterBand(3);
        }
        //RGBA = 4 bands
        else if (_warpedDS->GetRasterCount() == 4)
        {
            bandRed = _warpedDS->GetRasterBand(1);
            bandGreen = _warpedDS->GetRasterBand(2);
            bandBlue = _warpedDS->GetRasterBand(3);
            bandAlpha = _warpedDS->GetRasterBand(4);
        }
        //Gray = 1 band
        else if (_warpedDS->GetRasterCount() == 1)
        {
            bandGray = _warpedDS->GetRasterBand(1);
        }
        //Gray + alpha = 2 bands
        else if (_warpedDS->GetRasterCount() == 2)
        {
            bandGray = _warpedDS->GetRasterBand(1);
            bandAlpha = _warpedDS->GetRasterBand(2);
        }
    }



    //The pixel format is always RGBA to support transparency
    Image::PixelFormat pixelFormat = Image::R8G8B8A8_UNORM;


    if (bandRed && bandGreen && bandBlue)
    {
        unsigned char *red = new unsigned char[target_width * target_height];
        unsigned char *green = new unsigned char[target_width * target_height];
        unsigned char *blue = new unsigned char[target_width * target_height];
        unsigned char *alpha = new unsigned char[target_width * target_height];

        //Initialize the alpha values to 255.
        memset(alpha, 255, target_width * target_height);

        image = Image::create(
            pixelFormat,
            tileSize,
            tileSize);

        memset(image->data<char>(), 0, image->sizeInBytes());

        //image = new osg::Image;
        //image->allocateImage(tileSize, tileSize, 1, pixelFormat, GL_UNSIGNED_BYTE);
        //memset(image->data(), 0, image->getImageSizeInBytes());

        rasterIO(bandRed, GF_Read, off_x, off_y, width, height, red, target_width, target_height, GDT_Byte, 0, 0, gdalOptions().interpolation);
        rasterIO(bandGreen, GF_Read, off_x, off_y, width, height, green, target_width, target_height, GDT_Byte, 0, 0, gdalOptions().interpolation);
        rasterIO(bandBlue, GF_Read, off_x, off_y, width, height, blue, target_width, target_height, GDT_Byte, 0, 0, gdalOptions().interpolation);

        if (bandAlpha)
        {
            rasterIO(bandAlpha, GF_Read, off_x, off_y, width, height, alpha, target_width, target_height, GDT_Byte, 0, 0, gdalOptions().interpolation);
        }

        for (int src_row = 0, dst_row = tile_offset_top;
            src_row < target_height;
            src_row++, dst_row++)
        {
            unsigned int flippedRow = tileSize - dst_row - 1;
            for (int src_col = 0, dst_col = tile_offset_left;
                src_col < target_width;
                ++src_col, ++dst_col)
            {
                //u8vec4 c = u8vec4(
                //    red[src_col + src_row * target_width],
                //    green[src_col + src_row * target_width],
                //    blue[src_col + src_row * target_width],
                //    alpha[src_col + src_row * target_width]);

                int i = src_col + src_row * target_width;
                fvec4 c = fvec4(red[i], green[i], blue[i], alpha[i]) / 255.0f;

                if (!isValidValue(c.r, bandRed) ||
                    !isValidValue(c.g, bandGreen) ||
                    !isValidValue(c.b, bandBlue) ||
                    (bandAlpha && !isValidValue(c.a, bandAlpha)))
                {
                    c.a = 0.0f;
                }

                image->write(c, dst_col, flippedRow);

#if 0
                (image->data<unsigned char>(dst_col, flippedRow) + 0) = r;
                (image->data<unsigned char>(dst_col, flippedRow) + 1) = g;
                (image->data<unsigned char>(dst_col, flippedRow) + 2) = b;
                if (!isValidValue(r, bandRed) ||
                    !isValidValue(g, bandGreen) ||
                    !isValidValue(b, bandBlue) ||
                    (bandAlpha && !isValidValue(a, bandAlpha)))
                {
                    a = 0.0f;
                }
                *(image->data(dst_col, flippedRow) + 3) = a;
#endif
            }
        }

        delete[] red;
        delete[] green;
        delete[] blue;
        delete[] alpha;
    }
    else if (bandGray)
    {
        if (isCoverage)
        {
            GDALDataType gdalDataType = bandGray->GetRasterDataType();

            int gdalSampleSize =
                (gdalDataType == GDT_Byte) ? 1 :
                (gdalDataType == GDT_UInt16 || gdalDataType == GDT_Int16) ? 2 :
                4;

            // Create an un-normalized image to hold coverage values.
            //image = LandCover::createImage(tileSize);
            image = Image::create(
                Image::R16_UNORM,
                tileSize,
                tileSize);

            // initialize all coverage texels to NODATA. -gw
            image->fill(fvec4(NO_DATA_VALUE));

            // coverage data; one channel data that is not subject to interpolated values
            unsigned char* data = new unsigned char[target_width * target_height * gdalSampleSize];
            memset(data, 0, target_width * target_height * gdalSampleSize);

            fvec4 temp(0.0f);

            int success;
            float nodata = bandGray->GetNoDataValue(&success);
            if (!success)
                nodata = NO_DATA_VALUE; //getNoDataValue(); //getOptions().noDataValue().get();

            if (rasterIO(bandGray, GF_Read, off_x, off_y, width, height, data, target_width, target_height, gdalDataType, 0, 0, Image::NEAREST))
            {
                // copy from data to image.
                for (int src_row = 0, dst_row = tile_offset_top; src_row < target_height; src_row++, dst_row++)
                {
                    unsigned int flippedRow = tileSize - dst_row - 1;
                    for (int src_col = 0, dst_col = tile_offset_left; src_col < target_width; ++src_col, ++dst_col)
                    {
                        unsigned char* ptr = &data[(src_col + src_row * target_width)*gdalSampleSize];

                        float value =
                            gdalSampleSize == 1 ? (float)(*ptr) :
                            gdalSampleSize == 2 ? (float)*(unsigned short*)ptr :
                            gdalSampleSize == 4 ? *(float*)ptr :
                            NO_DATA_VALUE;

                        if (!isValidValue(value, bandGray))
                            value = NO_DATA_VALUE;

                        temp.r = value;
                        image->write(temp, dst_col, flippedRow);
                        //write(temp, dst_col, flippedRow);
                    }
                }
            }
            else // err != CE_None
            {
                ROCKY_WARN << LC << "RasterIO failed.\n";
                // TODO - handle error condition
            }

            delete[] data;
        }

        else // greyscale image (not a coverage)
        {
            unsigned char *gray = new unsigned char[target_width * target_height];
            unsigned char *alpha = new unsigned char[target_width * target_height];

            //Initialize the alpha values to 255.
            memset(alpha, 255, target_width * target_height);

            image = Image::create(
                pixelFormat,
                tileSize,
                tileSize);

            memset(image->data<char>(), 0, image->sizeInBytes());

            //image = new osg::Image;
            //image->allocateImage(tileSize, tileSize, 1, pixelFormat, GL_UNSIGNED_BYTE);
            //memset(image->data(), 0, image->getImageSizeInBytes());

            rasterIO(bandGray, GF_Read, off_x, off_y, width, height, gray, target_width, target_height, GDT_Byte, 0, 0, gdalOptions().interpolation);

            if (bandAlpha)
            {
                rasterIO(bandAlpha, GF_Read, off_x, off_y, width, height, alpha, target_width, target_height, GDT_Byte, 0, 0, gdalOptions().interpolation);
            }

            for (int src_row = 0, dst_row = tile_offset_top;
                src_row < target_height;
                src_row++, dst_row++)
            {
                unsigned int flippedRow = tileSize - dst_row - 1;
                for (int src_col = 0, dst_col = tile_offset_left;
                    src_col < target_width;
                    ++src_col, ++dst_col)
                {
                    fvec4 c;
                    c.r = c.g = c.b = gray[src_col + src_row * target_width];
                    c.a = alpha[src_col + src_row * target_width];

                    if (!isValidValue(c.r, bandGray) ||
                        (bandAlpha && !isValidValue(c.a, bandAlpha)))
                    {
                        c.a = 0.0f;
                    }

                    c /= 255.0f;

                    image->write(c, dst_col, flippedRow);

                    //unsigned char g = gray[src_col + src_row * target_width];
                    //unsigned char a = alpha[src_col + src_row * target_width];
                    //*(image->data(dst_col, flippedRow) + 0) = g;
                    //*(image->data(dst_col, flippedRow) + 1) = g;
                    //*(image->data(dst_col, flippedRow) + 2) = g;
                    //*(image->data(dst_col, flippedRow) + 3) = a;
                }
            }

            delete[]gray;
            delete[]alpha;
        }
    }
    else if (bandPalette)
    {
        //Palette indexed imagery doesn't support interpolation currently and only uses nearest
        //b/c interpolating palette indexes doesn't make sense.
        unsigned char *palette = new unsigned char[target_width * target_height];

        //image = new osg::Image;

        if (isCoverage == true)
        {
            //image = LandCover::createImage(tileSize);
            image = Image::create(
                Image::R32_SFLOAT,
                tileSize,
                tileSize);

            image->fill(fvec4(NO_DATA_VALUE));

            // initialize all coverage texels to NODATA. -gw
            //ImageUtils::PixelWriter write(image.get());
            //write.assign(Color(NO_DATA_VALUE));
        }
        else
        {
            image = Image::create(
                pixelFormat,
                tileSize,
                tileSize);

            memset(image->data<unsigned char>(), 0, image->sizeInBytes());
        }

        rasterIO(
            bandPalette, 
            GF_Read,
            off_x, off_y,
            width, height, 
            palette,
            target_width, target_height,
            GDT_Byte, 0, 0, 
            Image::NEAREST);

        //ImageUtils::PixelWriter write(image.get());

        Image::Pixel pixel;

        for (int src_row = 0, dst_row = tile_offset_top;
            src_row < target_height;
            src_row++, dst_row++)
        {
            unsigned int flippedRow = tileSize - dst_row - 1;
            for (int src_col = 0, dst_col = tile_offset_left;
                src_col < target_width;
                ++src_col, ++dst_col)
            {
                unsigned char p = palette[src_col + src_row * target_width];

                if (isCoverage)
                {
                    if (_gdalOptions.coverageUsesPaletteIndex == true)
                    {
                        pixel.r = (float)p;
                    }
                    else
                    {
                        u8vec4 color;
                        if (getPalleteIndexColor(bandPalette, p, color) &&
                            isValidValue((float)color.r, bandPalette)) // need this?
                        {
                            pixel.r = (float)color.r;
                        }
                        else
                        {
                            pixel.r = NO_DATA_VALUE;
                        }
                    }

                    fvec4 fpixel = fvec4(pixel) / 255.0f;

                    image->write(fpixel, dst_col, flippedRow);
                }
                else
                {
                    u8vec4 color;
                    if (!getPalleteIndexColor(bandPalette, p, color))
                    {
                        color.a = 0.0f;
                    }
                    else if (!isValidValue((float)color.r, bandPalette)) // is this applicable for palettized data?
                    {
                        color.a = 0.0f;
                    }

                    fvec4 fcolor = fvec4(color) / 255.0f;
                    image->write(fcolor, dst_col, flippedRow);
                    //*(image->data(dst_col, flippedRow) + 0) = color.r();
                    //*(image->data(dst_col, flippedRow) + 1) = color.g();
                    //*(image->data(dst_col, flippedRow) + 2) = color.b();
                    //*(image->data(dst_col, flippedRow) + 3) = color.a();
                }
            }
        }

        delete[] palette;

    }
    else
    {
        ROCKY_WARN
            << LC << "Could not find red, green and blue bands or gray bands in "
            << gdalOptions().url->full()
            << ".  Cannot create image. " << std::endl;

        return { };
    }

    return image;
}

shared_ptr<Heightfield>
GDAL::Driver::createHeightfield(
    const TileKey& key,
    unsigned tileSize,
    const IOOptions& io)
{
    if (_maxDataLevel.isSet() && key.getLevelOfDetail() > _maxDataLevel.get())
    {
        //ROCKY_NOTICE << "Reached maximum data resolution key=" << key.getLevelOfDetail() << " max=" << _maxDataLevel <<  std::endl;
        return NULL;
    }

    //GDAL_SCOPED_LOCK;

    //Allocate the heightfield
    auto hf = Heightfield::create(tileSize, tileSize);

    //osg::ref_ptr<osg::HeightField> hf = new osg::HeightField;
    //hf->allocate(tileSize, tileSize);

    if (intersects(key))
    {
        //Get the meter extents of the tile
        double xmin, ymin, xmax, ymax;
        key.getExtent().getBounds(xmin, ymin, xmax, ymax);

        // Try to find a FLOAT band
        GDALRasterBand* band = findBandByDataType(_warpedDS, GDT_Float32);
        if (band == NULL)
        {
            // Just get first band
            band = _warpedDS->GetRasterBand(1);
        }

        if (gdalOptions().interpolation == Image::NEAREST)
        {
            double colMin, colMax;
            double rowMin, rowMax;
            geoToPixel(xmin, ymin, colMin, rowMax);
            geoToPixel(xmax, ymax, colMax, rowMin);
            std::vector<float> buffer(tileSize * tileSize, NO_DATA_VALUE);

            int iColMin = floor(colMin);
            int iColMax = ceil(colMax);
            int iRowMin = floor(rowMin);
            int iRowMax = ceil(rowMax);
            int iNumCols = iColMax - iColMin + 1;
            int iNumRows = iRowMax - iRowMin + 1;

            int iWinColMin = std::max(0, iColMin);
            int iWinColMax = std::min(_warpedDS->GetRasterXSize() - 1, iColMax);
            int iWinRowMin = std::max(0, iRowMin);
            int iWinRowMax = std::min(_warpedDS->GetRasterYSize() - 1, iRowMax);
            int iNumWinCols = iWinColMax - iWinColMin + 1;
            int iNumWinRows = iWinRowMax - iWinRowMin + 1;

            int iBufColMin = std::round((iWinColMin - iColMin) / double(iNumCols - 1) * (tileSize - 1));
            int iBufColMax = std::round((iWinColMax - iColMin) / double(iNumCols - 1) * (tileSize - 1));
            int iBufRowMin = std::round((iWinRowMin - iRowMin) / double(iNumRows - 1) * (tileSize - 1));
            int iBufRowMax = std::round((iWinRowMax - iRowMin) / double(iNumRows - 1) * (tileSize - 1));
            int iNumBufCols = iBufColMax - iBufColMin + 1;
            int iNumBufRows = iBufRowMax - iBufRowMin + 1;

            int startOffset = iBufRowMin * tileSize + iBufColMin;
            int lineSpace = tileSize * sizeof(float);

            rasterIO(band, GF_Read, iWinColMin, iWinRowMin, iNumWinCols, iNumWinRows, &buffer[startOffset], iNumBufCols, iNumBufRows, GDT_Float32, 0, lineSpace);

            for (unsigned r = 0, ir = tileSize - 1; r < tileSize; ++r, --ir)
            {
                for (unsigned c = 0; c < tileSize; ++c)
                {
                    hf->heightAt(c, ir) = _linearUnits * buffer[r * tileSize + c];
                }
            }
        }
        else
        {
            double dx = (xmax - xmin) / (tileSize - 1);
            double dy = (ymax - ymin) / (tileSize - 1);
            for (unsigned r = 0; r < tileSize; ++r)
            {
                double geoY = ymin + (dy * (double)r);
                for (unsigned c = 0; c < tileSize; ++c)
                {
                    double geoX = xmin + (dx * (double)c);
                    float h = getInterpolatedValue(band, geoX, geoY) * _linearUnits;
                    hf->heightAt(c, r) = h;
                    //hf->setHeight(c, r, h);
                }
            }
        }
    }
    else
    {
        hf->fill(NO_DATA_VALUE);
        //std::vector<float>& heightList = hf->getHeightList();
        //std::fill(heightList.begin(), heightList.end(), NO_DATA_VALUE);
    }
    return hf;
}

shared_ptr<Heightfield>
GDAL::Driver::createHeightfieldWithVRT(
    const TileKey& key,
    unsigned tileSize,
    const IOOptions& io)
{
    if (_maxDataLevel.isSet() && key.getLevelOfDetail() > _maxDataLevel.get())
    {
        return nullptr;
    }

    //GDAL_SCOPED_LOCK;

    //Allocate the heightfield
    auto hf = Heightfield::create(tileSize, tileSize);
    hf->fill(NO_DATA_VALUE);

    //osg::ref_ptr<osg::HeightField> hf = new osg::HeightField;
    //hf->allocate(tileSize, tileSize);
    //for (unsigned int i = 0; i < hf->getHeightList().size(); ++i) hf->getHeightList()[i] = NO_DATA_VALUE;

    if (intersects(key))
    {
        GDALResampleAlg resampleAlg = GRA_CubicSpline;
        switch (_gdalOptions.interpolation)
        {
        case Image::NEAREST:
            resampleAlg = GRA_NearestNeighbour;
            break;
        case Image::AVERAGE:
            resampleAlg = GRA_Average;
            break;
        case Image::BILINEAR:
            resampleAlg = GRA_Bilinear;
            break;
        case Image::CUBIC:
            resampleAlg = GRA_Cubic;
            break;
        case Image::CUBICSPLINE:
            resampleAlg = GRA_CubicSpline;
            break;
        }

        // Create warp options
        GDALWarpOptions* psWarpOptions = GDALCreateWarpOptions();
        psWarpOptions->eResampleAlg = resampleAlg;
        psWarpOptions->hSrcDS = _srcDS;
        psWarpOptions->nBandCount = _srcDS->GetRasterCount();
        psWarpOptions->panSrcBands =
            (int*)CPLMalloc(sizeof(int) * psWarpOptions->nBandCount);
        psWarpOptions->panDstBands =
            (int*)CPLMalloc(sizeof(int) * psWarpOptions->nBandCount);

        for (short unsigned int i = 0; i < psWarpOptions->nBandCount; ++i) {
            psWarpOptions->panDstBands[i] = psWarpOptions->panSrcBands[i] = i + 1;
        }

        // Create the image to image transformer
        void* transformerArg = GDALCreateGenImgProjTransformer2(_srcDS, NULL, NULL);
        if (transformerArg == NULL) {
            GDALDestroyWarpOptions(psWarpOptions);
            // ERROR;
            return 0;
        }

        // Expanded
        double resolution = key.getExtent().width() / ((double)tileSize - 1);
        double adfGeoTransform[6];
        adfGeoTransform[0] = key.getExtent().xMin() - resolution;
        adfGeoTransform[1] = resolution;
        adfGeoTransform[2] = 0;
        adfGeoTransform[3] = key.getExtent().yMax() + resolution;
        adfGeoTransform[4] = 0;
        adfGeoTransform[5] = -resolution;

        // Specify the destination geotransform
        GDALSetGenImgProjTransformerDstGeoTransform(transformerArg, adfGeoTransform);

        psWarpOptions->pTransformerArg = transformerArg;
        psWarpOptions->pfnTransformer = GDALGenImgProjTransform;

        GDALDatasetH tileDS = GDALCreateWarpedVRT(_srcDS, tileSize, tileSize, adfGeoTransform, psWarpOptions);

        GDALSetProjection(tileDS, key.getProfile()->getSRS()->getWKT().c_str());

        resolution = key.getExtent().width() / ((double)tileSize);
        adfGeoTransform[0] = key.getExtent().xMin();
        adfGeoTransform[1] = resolution;
        adfGeoTransform[2] = 0;
        adfGeoTransform[3] = key.getExtent().yMax();
        adfGeoTransform[4] = 0;
        adfGeoTransform[5] = -resolution;

        // Set the geotransform back to what it should actually be.
        GDALSetGeoTransform(tileDS, adfGeoTransform);

        float* heights = new float[tileSize * tileSize];
        for (unsigned int i = 0; i < tileSize * tileSize; i++)
        {
            heights[i] = NO_DATA_VALUE;
        }
        GDALRasterBand* band = static_cast<GDALRasterBand*>(GDALGetRasterBand(tileDS, 1));
        band->RasterIO(GF_Read, 0, 0, tileSize, tileSize, heights, tileSize, tileSize, GDT_Float32, 0, 0);

        hf = Heightfield::create(tileSize, tileSize);
        //hf = new osg::HeightField();
        //hf->allocate(tileSize, tileSize);
        for (unsigned int c = 0; c < tileSize; c++)
        {
            for (unsigned int r = 0; r < tileSize; r++)
            {
                unsigned inv_r = tileSize - r - 1;
                float h = heights[r * tileSize + c];
                if (!isValidValue(h, band))
                {
                    h = NO_DATA_VALUE;
                }
                hf->heightAt(c, inv_r) = h;
                //hf->setHeight(c, inv_r, h);
            }
        }

        delete[] heights;

        // Close the dataset
        if (tileDS != NULL)
        {
            GDALClose(tileDS);
        }

        // Destroy the warp options
        if (psWarpOptions != NULL)
        {
            GDALDestroyWarpOptions(psWarpOptions);
        }

        // Note:  The transformer is closed in the warped dataset so we don't need to free it ourselves.
    }
    return hf;
}
//...................................................................

void
GDAL::Options::readFrom(const Config& conf)
{
    interpolation.setDefault(Image::AVERAGE);
    useVRT.setDefault(false);
    coverageUsesPaletteIndex.setDefault(true);
    singleThreaded.setDefault(false);

    conf.get("url", url);
    conf.get("uri", url);
    conf.get("connection", connection);
    conf.get("subdataset", subDataSet);
    conf.get("interpolation", "nearest", interpolation, Image::NEAREST);
    conf.get("interpolation", "average", interpolation, Image::AVERAGE);
    conf.get("interpolation", "bilinear", interpolation, Image::BILINEAR);
    conf.get("interpolation", "cubic", interpolation, Image::CUBIC);
    conf.get("interpolation", "cubicspline", interpolation, Image::CUBICSPLINE);
    conf.get("coverage_uses_palette_index", coverageUsesPaletteIndex);
    conf.get("single_threaded", singleThreaded);

    // report on deprecated usage
    const std::string deprecated_keys[] = {
        "use_vrt",
        "warp_profile"
    };
    for (const auto& key : deprecated_keys)
        if (conf.hasValue(key))
            ROCKY_INFO << "Deprecated property \"" << key << "\" ignored" << std::endl;
}

void
GDAL::Options::writeTo(Config& conf) const
{
    conf.set("url", url);
    conf.set("connection", connection);
    conf.set("subdataset", subDataSet);
    conf.set("interpolation", "nearest", interpolation, Image::NEAREST);
    conf.set("interpolation", "average", interpolation, Image::AVERAGE);
    conf.set("interpolation", "bilinear", interpolation, Image::BILINEAR);
    conf.set("interpolation", "cubic", interpolation, Image::CUBIC);
    conf.set("interpolation", "cubicspline", interpolation, Image::CUBICSPLINE);
    conf.set("coverage_uses_palette_index", coverageUsesPaletteIndex);
    conf.set("single_threaded", singleThreaded);
}

void GDAL::LayerBase::setURI(const URI& value) {
    _options.url = value;
}
const URI& GDAL::LayerBase::getURL() const {
    return _options.url;
}
void GDAL::LayerBase::setConnection(const std::string& value) {
    _options.connection = value;
}
const std::string& GDAL::LayerBase::getConnection() const {
    return _options.connection;
}
void GDAL::LayerBase::setSubDataSet(unsigned value) {
    _options.subDataSet = value;
}
unsigned GDAL::LayerBase::getSubDataSet() const {
    return _options.subDataSet;
}
void GDAL::LayerBase::setInterpolation(const Image::Interpolation& value) {
    _options.interpolation = value;
}
const Image::Interpolation& GDAL::LayerBase::getInterpolation() const {
    return _options.interpolation;
}
void GDAL::LayerBase::setUseVRT(bool value) {
    _options.useVRT = value;
}
bool GDAL::LayerBase::getUseVRT() const {
    return _options.useVRT;
}

//......................................................................

#undef LC
#define LC "[GDAL] \"" << getName() << "\" "

namespace
{
    template<typename T>
    Status openOnThisThread(
        const T* layer,
        const GDAL::Options& options,
        GDAL::Driver::Ptr& driver,
        shared_ptr<Profile>* profile,
        DataExtentList* out_dataExtents,
        const IOOptions& io)
    {
        driver = std::make_shared<GDAL::Driver>();

        auto elevationLayer = dynamic_cast<const ElevationLayer*>(layer);
        if (elevationLayer)
        {
            if (elevationLayer->noDataValue().has_value())
                driver->setNoDataValue(elevationLayer->noDataValue());
            if (elevationLayer->minValidValue().has_value())
                driver->setMinValidValue(elevationLayer->minValidValue());
            if (elevationLayer->maxValidValue().has_value())
                driver->setMaxValidValue(elevationLayer->maxValidValue());
        }

        if (layer->maxDataLevel().has_value())
            driver->setMaxDataLevel(layer->maxDataLevel());

        Status status = driver->open(
            layer->name(),
            options,
            layer->tileSize(),
            out_dataExtents,
            io);

        if (status.failed())
            return status;

        if (driver->getProfile() && profile != nullptr)
        {
            *profile = driver->getProfile();
        }

        return Status::NoError;
    }
}

//......................................................................

//Config
//GDALImageLayer::Options::getConfig() const
//{
//    Config conf = ImageLayer::Options::getConfig();
//    writeTo(conf);
//    return conf;
//}
//
//void
//GDALImageLayer::Options::fromConfig(const Config& conf)
//{
//    readFrom(conf);
//}

//......................................................................

//REGISTER_OSGEARTH_LAYER(gdalimage, GDALImageLayer);


GDALImageLayer::GDALImageLayer() :
    super()
{
    construct(Config());
}

GDALImageLayer::GDALImageLayer(const Config& conf) :
    super(conf)
{
    construct(conf);
}

void
GDALImageLayer::construct(const Config& conf)
{
    _options.readFrom(conf);

    setRenderType(RENDERTYPE_TERRAIN_SURFACE);
}

Config
GDALImageLayer::getConfig() const
{
    Config conf = ImageLayer::getConfig();
    _options.writeTo(conf);
    return conf;
}

Status
GDALImageLayer::openImplementation(const IOOptions& io)
{
    Status parent = ImageLayer::openImplementation(io);
    if (parent.failed())
        return parent;

    shared_ptr<Profile> profile;

    // GDAL thread-safety requirement: each thread requires a separate GDALDataSet.
    // So we just encapsulate the entire setup once per thread.
    // https://trac.osgeo.org/gdal/wiki/FAQMiscellaneous#IstheGDALlibrarythread-safe

    GDAL::Driver::Ptr& driver = _drivers.get();

    DataExtentList dataExtents;

    Status s = openOnThisThread(
        this,
        _options,
        driver,
        &profile,
        &dataExtents,
        io);

    if (s.failed())
        return s;

    // if the driver generated a valid profile, set it.
    if (profile)
    {
        setProfile(profile);
    }

    setDataExtents(dataExtents);

    return s;
}

Status
GDALImageLayer::closeImplementation()
{
    // safely shut down all per-thread handles.
    _drivers.clear();

    return ImageLayer::closeImplementation();
}

Result<GeoImage>
GDALImageLayer::createImageImplementation(
    const TileKey& key,
    const IOOptions& io) const
{
    if (getStatus().failed())
        return Result(GeoImage::INVALID);

    if (isClosing() || !isOpen())
        return Result(GeoImage::INVALID);

    GDAL::Driver::Ptr& driver = _drivers.get();
    if (driver == nullptr)
    {
        // calling openImpl with NULL params limits the setup
        // since we already called this during openImplementation
        openOnThisThread(
            this,
            _options,
            driver,
            nullptr,
            nullptr,
            io);
    }

    if (driver)
    {
        auto image = driver->createImage(
            key,
            _tileSize,
            _coverage == true,
            io);

        return Result(GeoImage(image, key.getExtent()));
    }

    return Result(GeoImage::INVALID);
}

//......................................................................

#if 0
Config
GDALElevationLayer::Options::getConfig() const
{
    Config conf = ElevationLayer::Options::getConfig();
    writeTo(conf);
    return conf;
}

void
GDALElevationLayer::Options::fromConfig(const Config& conf)
{
    readFrom(conf);
}
#endif

//......................................................................

//REGISTER_OSGEARTH_LAYER(gdalelevation, GDALElevationLayer);

GDALElevationLayer::GDALElevationLayer() :
    super()
{
    construct(Config());
}

GDALElevationLayer::GDALElevationLayer(const Config& conf) :
    super(conf)
{
    construct(conf);
}

void
GDALElevationLayer::construct(const Config& conf)
{
    _options.readFrom(conf);
}

Config
GDALElevationLayer::getConfig() const
{
    Config conf = ElevationLayer::getConfig();
    _options.writeTo(conf);
    return conf;
}

Status
GDALElevationLayer::openImplementation(const IOOptions& io)
{
    Status parent = ElevationLayer::openImplementation(io);
    if (parent.failed())
        return parent;

    shared_ptr<Profile> profile;

    // GDAL thread-safety requirement: each thread requires a separate GDALDataSet.
    // So we just encapsulate the entire setup once per thread.
    // https://trac.osgeo.org/gdal/wiki/FAQMiscellaneous#IstheGDALlibrarythread-safe

    // Open the dataset temporarily to query the profile and extents.
    GDAL::Driver::Ptr driver = _drivers.get();

    DataExtentList dataExtents;

    Status s = openOnThisThread(
        this,
        _options,
        driver,
        &profile,
        &dataExtents,
        io);

    if (s.failed())
        return s;

    if (profile)
        setProfile(profile);

    setDataExtents(dataExtents);


    return s;
}

Status
GDALElevationLayer::closeImplementation()
{
    // safely shut down all per-thread handles.
    _drivers.clear();
    return ElevationLayer::closeImplementation();
}

Result<GeoHeightfield>
GDALElevationLayer::createHeightfieldImplementation(
    const TileKey& key,
    const IOOptions& io) const
{
    if (getStatus().failed())
        return Result<GeoHeightfield>(getStatus());

    // check while locked to ensure we may continue
    if (isClosing() || !isOpen())
        return GeoHeightfield::INVALID;

    GDAL::Driver::Ptr& driver = _drivers.get();
    if (driver == nullptr)
    {
        // calling openImpl with NULL params limits the setup
        // since we already called this during openImplementation
        openOnThisThread(
            this,
            _options,
            driver,
            nullptr,
            nullptr,
            io);
    }

    if (driver)
    {
        shared_ptr<Heightfield> heightfield;

        if (_options.useVRT == true)
        {
            heightfield = driver->createHeightfieldWithVRT(
                key,
                tileSize(),
                io);
        }
        else
        {
            heightfield = driver->createHeightfield(
                key,
                tileSize(),
                io);
        }

        return GeoHeightfield(heightfield, key.getExtent());
    }

    return GeoHeightfield::INVALID;
}

//...................................................................


#undef LC
#define LC "[GDAL] "

namespace
{
    shared_ptr<Image> createImageFromDataset(GDALDataset* ds)
    {
        // called internally -- GDAL lock not required

        int numBands = ds->GetRasterCount();
        if (numBands < 1)
            return nullptr;

        Image::PixelFormat format;
        //GLenum dataType;
        int    sampleSize;
        //GLint  internalFormat;

        switch (ds->GetRasterBand(1)->GetRasterDataType())
        {
        case GDT_Byte:
            sampleSize = 1;
            format =
                numBands == 1 ? Image::R8_UNORM :
                numBands == 2 ? Image::R8G8_UNORM :
                numBands == 3 ? Image::R8G8B8_UNORM :
                Image::R8G8B8A8_UNORM;
            //dataType = GL_UNSIGNED_BYTE;
            //internalFormat = GL_R8;
            break;
        case GDT_UInt16:
        case GDT_Int16:
            sampleSize = 2;
            format = Image::R16_UNORM;
            //dataType = GL_UNSIGNED_SHORT;
            //internalFormat = GL_R16;
            break;
        default:
            format = Image::R32_SFLOAT;
            sampleSize = 4;
            //dataType = GL_FLOAT;
            //internalFormat = GL_R32F; // GL_LUMINANCE32F_ARB;
        }

        //GLenum pixelFormat =
        //    numBands == 1 ? GL_RED :
        //    numBands == 2 ? GL_RG :
        //    numBands == 3 ? GL_RGB :
        //    GL_RGBA;

        int pixelBytes = sampleSize * numBands;

        //Allocate the image
        auto image = Image::create(
            format,
            ds->GetRasterXSize(),
            ds->GetRasterYSize());

        //osg::Image *image = new osg::Image;
        //image->allocateImage(ds->GetRasterXSize(), ds->GetRasterYSize(), 1, pixelFormat, dataType);

        CPLErr err = ds->RasterIO(
            GF_Read,
            0, 0,
            image->width(), image->height(),
            (void*)image->data<void>(),
            image->width(), image->height(),
            ds->GetRasterBand(1)->GetRasterDataType(),
            numBands,
            nullptr,
            pixelBytes,
            pixelBytes * image->width(),
            1);
        if (err != CE_None)
        {
            ROCKY_WARN << LC << "RasterIO failed.\n";
        }

        ds->FlushCache();

        image->flipVerticalInPlace();

        return image;
    }

    GDALDataset* createMemDS(int width, int height, int numBands, GDALDataType dataType, double minX, double minY, double maxX, double maxY, const std::string &projection)
    {
        //Get the MEM driver
        GDALDriver* memDriver = (GDALDriver*)GDALGetDriverByName("MEM");
        if (!memDriver)
        {
            ROCKY_WARN << LC << "Could not get MEM driver" << std::endl;
            return NULL;
        }

        //Create the in memory dataset.
        GDALDataset* ds = memDriver->Create("", width, height, numBands, dataType, 0);
        if (!ds)
        {
            ROCKY_WARN << LC << "memDriver.create failed" << std::endl;
            return NULL;
        }

        //Initialize the color interpretation
        if (numBands == 1)
        {
            ds->GetRasterBand(1)->SetColorInterpretation(GCI_GrayIndex);
        }
        else
        {
            if (numBands >= 1)
                ds->GetRasterBand(1)->SetColorInterpretation(GCI_RedBand);
            if (numBands >= 2)
                ds->GetRasterBand(2)->SetColorInterpretation(GCI_GreenBand);
            if (numBands >= 3)
                ds->GetRasterBand(3)->SetColorInterpretation(GCI_BlueBand);
            if (numBands >= 4)
                ds->GetRasterBand(4)->SetColorInterpretation(GCI_AlphaBand);
        }

        //Initialize the geotransform
        double geotransform[6];
        double x_units_per_pixel = (maxX - minX) / (double)width;
        double y_units_per_pixel = (maxY - minY) / (double)height;
        geotransform[0] = minX;
        geotransform[1] = x_units_per_pixel;
        geotransform[2] = 0;
        geotransform[3] = maxY;
        geotransform[4] = 0;
        geotransform[5] = -y_units_per_pixel;
        ds->SetGeoTransform(geotransform);
        ds->SetProjection(projection.c_str());

        return ds;
    }

    GDALDataset* createDataSetFromImage(
        const Image* image,
        double minX, double minY, double maxX, double maxY,
        const std::string& projection)
    {
        //Clone the incoming image
        auto clonedImage = image->clone();
        //osg::ref_ptr<osg::Image> clonedImage = new osg::Image(*image);

        //Flip the image
        clonedImage->flipVerticalInPlace();

        auto b = image->componentSizeInBytes();
        GDALDataType gdalDataType =
            b == 1 ? GDT_Byte :
            b == 2 ? GDT_UInt16 :
            b == 4 ? GDT_Float32 :
            b == 8 ? GDT_Float64 :
            GDT_Byte;

        //GDALDataType gdalDataType =
        //    image->getDataType() == GL_UNSIGNED_BYTE ? GDT_Byte :
        //    image->getDataType() == GL_UNSIGNED_SHORT ? GDT_UInt16 :
        //    image->getDataType() == GL_FLOAT ? GDT_Float32 :
        //    GDT_Byte;

        int numBands = image->numComponents();
        //osg::Image::computeNumComponents(image->getPixelFormat());

        if (numBands == 0)
        {
            ROCKY_WARN << LC << "Failure in createDataSetFromImage: unsupported pixel format\n";
            return nullptr;
        }

        int pixelBytes =
            gdalDataType == GDT_Byte ? numBands :
            gdalDataType == GDT_UInt16 ? 2 * numBands :
            4 * numBands;

        GDALDataset* srcDS = createMemDS(
            image->width(), image->height(),
            numBands,
            gdalDataType,
            minX, minY, maxX, maxY,
            projection);

        if (srcDS)
        {
            CPLErr err = srcDS->RasterIO(
                GF_Write,
                0, 0,
                clonedImage->width(), clonedImage->height(),
                (void*)clonedImage->data<void>(),
                clonedImage->width(),
                clonedImage->height(),
                gdalDataType,
                numBands,
                NULL,
                pixelBytes,
                pixelBytes * image->width(),
                1);
            if (err != CE_None)
            {
                ROCKY_WARN << LC << "RasterIO failed.\n";
            }

            srcDS->FlushCache();
        }

        return srcDS;
    }
}

shared_ptr<Image>
rocky::GDAL::reprojectImage(
    const Image* srcImage,
    const std::string srcWKT,
    double srcMinX, double srcMinY, double srcMaxX, double srcMaxY,
    const std::string destWKT,
    double destMinX, double destMinY, double destMaxX, double destMaxY,
    int width,
    int height,
    bool useBilinearInterpolation)
{
    // Unnessecary since this is totally self-contained with thread-safe DataSets
    //GDAL_SCOPED_LOCK;

    //Create a dataset from the source image
    GDALDataset* srcDS = createDataSetFromImage(
        srcImage, srcMinX, srcMinY, srcMaxX, srcMaxY, srcWKT);

    if (srcDS == nullptr)
        return nullptr;

    if (width == 0 || height == 0)
    {
        double outgeotransform[6];
        double extents[4];
        void* transformer = GDALCreateGenImgProjTransformer(srcDS, srcWKT.c_str(), NULL, destWKT.c_str(), 1, 0, 0);
        GDALSuggestedWarpOutput2(srcDS,
            GDALGenImgProjTransform, transformer,
            outgeotransform,
            &width,
            &height,
            extents,
            0);
        GDALDestroyGenImgProjTransformer(transformer);
    }

    int numBands = srcDS->GetRasterCount();
    GDALDataType dataType = srcDS->GetRasterBand(1)->GetRasterDataType();

    GDALDataset* destDS = createMemDS(width, height, numBands, dataType, destMinX, destMinY, destMaxX, destMaxY, destWKT);

    if (useBilinearInterpolation == true)
    {
        GDALReprojectImage(
            srcDS, nullptr,
            destDS, nullptr,
            GRA_Bilinear,
            0, 0, 0, 0, 0);
    }
    else
    {
        GDALReprojectImage(
            srcDS, nullptr,
            destDS, nullptr,
            GRA_NearestNeighbour,
            0, 0, 0, 0, 0);
    }

    auto result = createImageFromDataset(destDS);

    delete srcDS;
    delete destDS;

    return result;
}
