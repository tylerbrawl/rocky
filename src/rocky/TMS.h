/**
 * rocky c++
 * Copyright 2023 Pelican Mapping
 * MIT License
 */
#pragma once

#include <rocky/URI.h>
#include <rocky/Image.h>
#include <rocky/TileKey.h>
#include <rocky/Profile.h>

namespace ROCKY_NAMESPACE
{
    namespace TMS
    {
        struct TileFormat
        {
            unsigned width = 0u;
            unsigned height = 0u;
            std::string mimeType;
            std::string extension;
        };

        struct TileSet
        {
            std::string href;
            double unitsPerPixel = 0.0;
            unsigned order = 0u;
        };

        enum class ProfileType
        {
            UNKNOWN,
            GEODETIC,
            MERCATOR,
            LOCAL
        };

        struct ROCKY_EXPORT TileMap
        {
            std::string tileMapService;
            std::string version;
            std::string title;
            std::string abstract;
            std::string srsString;
            std::string vsrsString;
            double originX, originY;
            double minX, minY, maxX, maxY;
            std::vector<TileSet> tileSets;
            TileFormat format;
            std::string filename;
            unsigned minLevel = 0u;
            unsigned maxLevel = 99u;
            unsigned numTilesWide = 0u;
            unsigned numTilesHigh = 0u;
            ProfileType profileType = ProfileType::UNKNOWN;
            TimeStamp timestamp;
            DataExtentList dataExtents;

            bool valid() const;
            void computeMinMaxLevel();
            void computeNumTiles();
            Profile createProfile() const;
            std::string getURI(const TileKey& key, bool invertY) const;
            bool intersectsKey(const TileKey& key) const;
            void generateTileSets(unsigned numLevels);

            TileMap() { }

            TileMap(
                const std::string& url,
                const Profile& profile,
                const DataExtentList& dataExtents,
                const std::string& format,
                int tile_width,
                int tile_height);
        };

        struct TileMapEntry
        {
            std::string title;
            std::string href;
            std::string srs;
            std::string profile;
        };

        using TileMapEntries = std::list<TileMapEntry>;

        extern Result<TileMap> readTileMap(const URI& location, const IOOptions& io);

        extern TileMapEntries readTileMapEntries(const URI& location, const IOOptions& io);


        /**
         * Underlying TMS driver that does the actual TMS I/O
         */
        class ROCKY_EXPORT Driver
        {
        public:
            Status open(
                const URI& uri,
                Profile& profile,
                const std::string& format,
                bool isCoverage,
                DataExtentList& out_dataExtents,
                const IOOptions& io);

            void close();

            Result<shared_ptr<Image>> read(
                const URI& uri,
                const std::string& uri_suffix,
                const TileKey& key,
                bool invertY,
                const IOOptions& io) const;

            bool write(
                const URI& uri,
                const TileKey& key,
                shared_ptr<Image> image,
                bool invertY,
                IOOptions& io) const;

        private:
            TileMap _tileMap;
            bool _forceRGBWrites;
            bool _isCoverage;

            //bool resolveWriter(const std::string& format);
        };
    }
}
