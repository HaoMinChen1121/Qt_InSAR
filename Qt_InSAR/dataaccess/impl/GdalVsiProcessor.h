#ifndef GDALVSIPROCESSOR_H
#define GDALVSIPROCESSOR_H

#include <QString>

class GdalVsiProcessor
{
public:
    /// Register custom GDAL pixel functions (called once at startup).
    static void registerPixelFunctions();

    /// Process a /vsi path into a loadable raster file.
    /// For complex data (CInt16/CFloat32), creates an amplitude VRT.
    /// For real data, extracts to a GeoTIFF with overviews.
    /// @param vsiPath        e.g. "/vsizip/path/to/file.zip/dir/file.tiff"
    /// @param outputBasePath output path without extension (appends .vrt or .tif)
    /// @return path to the result file, or empty string on failure
    static QString process(const QString& vsiPath, const QString& outputBasePath);

private:
    GdalVsiProcessor() = delete;
};

#endif // GDALVSIPROCESSOR_H
