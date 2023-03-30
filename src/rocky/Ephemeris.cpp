/**
 * rocky c++
 * Copyright 2023 Pelican Mapping
 * MIT License
 */
#include "Ephemeris.h"
#include "DateTime.h"
#include "Ellipsoid.h"

using namespace ROCKY_NAMESPACE;

namespace
{
    // Astronomical Math
    // http://www.stjarnhimlen.se/comp/ppcomp.html

    #define nrad(X) { while( X >= TWO_PI ) X -= TWO_PI; while( X < 0.0 ) X += TWO_PI; }
    #define nrad2(X) { while( X <= -osg::PI ) X += TWO_PI; while( X > osg::PI ) X -= TWO_PI; }

    static const double TWO_PI = (2.0 * M_PI);

    // http://www.stjarnhimlen.se/comp/tutorial.html#4
    // for reference only; same as DateTime::getJulianDay() - 2451544
    double dayNumber(int Y, int M, int D, double hoursUTC)
    {
        int d = 367 * Y - (7 * (Y + (((M + 9) / 12)))) / 4 + ((275 * M) / 9) + D - 730530;
        return (double)d + hoursUTC / 24.0;
    }

    double rev(double a)
    {
        return a - floor(a / 360.0) * 360.0;
    }

    struct Sun
    {
        // http://www.stjarnhimlen.se/comp/tutorial.html#5
        // Test: http://www.satellite-calculations.com/Satellite/suncalc.htm
        CelestialBody position(const DateTime& dt) const
        {
            static const Ellipsoid WGS84;

            // Reference to 1999Dec31.0TDT
            const double JD_REFTIME = DateTime(1999, 12, 31, 0.0).getJulianDay();
            double d = dt.getJulianDay() - JD_REFTIME;

            double w = 282.9404 + 4.70935E-5 * d;
            const double a = 1.0;

            double e = 0.016709 - 1.151E-9 * d;
            double M = 356.0470 + 0.9856002585 * d;
            double oblecl = 23.4393 - 3.563E-7 * d;
            double L = rev(w + rev(M));

            double E = rev(M + rad2deg(e * sin(deg2rad(M)) * (1.0 + e * cos(deg2rad(M)))));
            double x = a * cos(deg2rad(E)) - e;
            double y = a * sin(deg2rad(rev(E))) * sqrt(1.0 - e * e);
            double r = sqrt(x * x + y * y);
            double v = rad2deg(atan2(y, x));
            double sunlon = rev(v + w);

            x = r * cos(deg2rad(sunlon));
            y = r * sin(deg2rad(sunlon));
            double z = 0;

            double xequat = x;
            double yequat = y * cos(deg2rad(oblecl)) + z * sin(deg2rad(oblecl));
            double zequat = y * sin(deg2rad(oblecl)) + z * cos(deg2rad(oblecl));

            double RA_deg = rev(rad2deg(atan2(yequat, xequat)));
            double DECL_deg = rad2deg(atan2(zequat, sqrt(xequat * xequat + yequat * yequat)));

            double GMST0_deg = rev(L + 180);
            double UT = d - floor(d);

            CelestialBody sun;

            sun.rightAscension.set(RA_deg, Units::DEGREES);
            sun.declination.set(DECL_deg, Units::DEGREES);
            sun.latitude.set(DECL_deg, Units::DEGREES);
            sun.longitude.set(rev(0 * 180 + RA_deg - GMST0_deg - UT * 360), Units::DEGREES);
            sun.altitude.set(149600000.0, Units::KILOMETERS);

#if 0
            // compute topographic measurements relative to observer position:
            if (sun._observer.isValid())
            {
                double siteLat_deg = sun._observer.y(); // ?
                double siteLon_deg = sun._observer.x(); // ?

                double SIDEREAL_deg = GMST0_deg + UT * 360 + siteLon_deg;
                double siteLon_rad = d2r(siteLon_deg);
                double siteLat_rad = d2r(siteLat_deg);
                double hourAngle_rad = d2r(SIDEREAL_deg - RA_deg);
                double DECL_rad = d2r(DECL_deg);
                x = cos(hourAngle_rad) * cos(DECL_rad);
                y = sin(hourAngle_rad) * cos(DECL_rad);
                z = sin(DECL_rad);
                double xhor = x * sin(siteLat_rad) - z * cos(siteLat_rad);
                double yhor = y;
                double zhor = x * cos(siteLat_rad) + z * sin(siteLat_rad);

                double elev = asin(zhor);
                double azim = atan2(yhor, zhor);
                azim = siteLat_deg < 0.0 ? azim + osg::PI : azim - osg::PI;
                nrad2(azim);

                sun._topoElevation.set(elev, Units::RADIANS);
                sun._topoAzimuth.set(azim, Units::RADIANS);
            }
#endif

            // geocentric:
            {
                sun.geocentric = WGS84.geodeticToGeocentric({
                    sun.longitude.as(Units::DEGREES),
                    sun.latitude.as(Units::DEGREES),
                    sun.altitude.as(Units::METERS)
                });
            }

            // ECI:
            {
                double RA = sun.rightAscension.as(Units::RADIANS);
                double DECL = sun.declination.as(Units::RADIANS);
                double R = sun.altitude.as(Units::METERS);
                sun.eci = {
                    R * cos(DECL) * cos(RA),
                    R * cos(DECL) * sin(RA),
                    R * sin(DECL)
                };
            }

            return sun;
        }
    };

    struct Moon
    {
        // Math: http://www.stjarnhimlen.se/comp/ppcomp.html
        // More: http://www.stjarnhimlen.se/comp/tutorial.html#7
        // Test: http://www.satellite-calculations.com/Satellite/suncalc.htm
        // Test: http://www.timeanddate.com/astronomy/moon/light.html
        CelestialBody position(const DateTime& dt) const
        {
            static const Ellipsoid WGS84;

            // Reference to 1999Dec31.0TDT
            const double JD_REFTIME = DateTime(1999, 12, 31, 0.0).getJulianDay();
            double d = dt.getJulianDay() - JD_REFTIME;
            double N = deg2rad(125.1228 - 0.0529538083 * d);  nrad(N);
            double i = deg2rad(5.1454);
            double w = deg2rad(318.0634 + 0.1643573223 * d);  nrad(w);
            double a = 60.2666;//  (Earth radii)
            double e = 0.054900;
            double M = deg2rad(115.3654 + 13.0649929509 * d); nrad(M);

            double E = M + e * sin(M) * (1.0 + e * cos(M));
            nrad(E);

            double Estart = E;
            double Eerror = 9;
            int iterations = 0;
            while (Eerror > 0.0005 && iterations < 20)
            {
                iterations++;
                double E0 = E;
                double E1 = E0 - (E0 - e * sin(E0) - M) / (1.0 - e * cos(E0));
                nrad(E1);
                E = E1;
                Eerror = E < E0 ? E0 - E : E - E0;
            }

            nrad(E);
            double x = a * (cos(E) - e);
            double y = a * (sqrt(1.0 - e * e) * sin(E));

            double v = atan2(y, x);
            double r = sqrt(x * x + y * y);

            //Compute the geocentric (Earth-centered) position of the moon in the ecliptic coordinate system
            double xeclip = r * (cos(N) * cos(v + w) - sin(N) * sin(v + w) * cos(i));
            double yeclip = r * (sin(N) * cos(v + w) + cos(N) * sin(v + w) * cos(i));
            double zeclip = r * (sin(v + w) * sin(i));

            // calculate the ecliptic latitude and longitude here
            double lonEcl = atan2(yeclip, xeclip); nrad(lonEcl);
            double latEcl = atan2(zeclip, sqrt(xeclip * xeclip + yeclip * yeclip));

            // add in the perturbations.
            double Ms = deg2rad(356.0470 + 0.9856002585 * d); //nrad(Ms); // sun mean anomaly
            double ws = deg2rad(282.9404 + 4.70935E-5 * d); //nrad(ws); // sun longitude of perihelion
            double Ls = ws + Ms;    nrad(Ls);  // sun mean longitude

            double Mm = M;                     // moon mean anomaly
            double Lm = N + w + Mm; nrad(Lm);  // moon mean longitude
            double D = Lm - Ls;     nrad(D);   // moon mean elongation
            double F = Lm - N;      //nrad(F); // moon argument of latitude

            lonEcl = lonEcl + deg2rad(
                +(-1.274) * sin(Mm - 2 * D)    // (Evection)
                + (+0.658) * sin(2 * D)         // (Variation)
                + (-0.186) * sin(Ms)          // (Yearly equation)
                + (-0.059) * sin(2 * Mm - 2 * D)
                + (-0.057) * sin(Mm - 2 * D + Ms)
                + (+0.053) * sin(Mm + 2 * D)
                + (+0.046) * sin(2 * D - Ms)
                + (+0.041) * sin(Mm - Ms)
                + (-0.035) * sin(D)           // (Parallactic equation)
                + (-0.031) * sin(Mm + Ms)
                + (-0.015) * sin(2 * F - 2 * D)
                + (+0.011) * sin(Mm - 4 * D)
            );

            latEcl = latEcl + deg2rad(
                +(-0.173) * sin(F - 2 * D)
                + (-0.055) * sin(Mm - F - 2 * D)
                + (-0.046) * sin(Mm + F - 2 * D)
                + (+0.033) * sin(F + 2 * D)
                + (+0.017) * sin(2 * Mm + F)
            );

            r = r +
                -0.58 * cos(Mm - 2 * D)
                - 0.46 * cos(2 * D);

            // convert to elliptic geocentric (unit)
            double xh = r * cos(lonEcl) * cos(latEcl);
            double yh = r * sin(lonEcl) * cos(latEcl);
            double zh = r * sin(latEcl);

            // and then to rectangular equatorial (unit)
            double ecl = deg2rad(23.4393 - 3.563E-7 * d); // obliquity of elliptic (tilt of earth)
            double xe = xh;
            double ye = yh * cos(ecl) - zh * sin(ecl);
            double ze = yh * sin(ecl) + zh * cos(ecl);

            // get the ra/decl:
            double RA = atan2(ye, xe); nrad(RA);
            double Decl = atan2(ze, sqrt(xe * xe + ye * ye));

            // finally, adjust for the time of day (rotation of the earth).
            double UT = 2.0 * M_PI * (d - floor(d));
            //double UT = d - floor(d); // 0..1
            double GMST0 = Ls + deg2rad(180.0); nrad(GMST0);

            // Note. The paper creates a "correction" called called "topographic RA/DECL"
            // based on an observer location. We're not using that here which is why the
            // longitude doesn't match up exactly with the test site.

            double earthLat = Decl;
            double earthLon = RA - GMST0 - UT;
            nrad(earthLon);

            // since r (distance to moon) is in "earth radius units", resolve it to meters
            r *= WGS84.semiMajorAxis();

            CelestialBody moon;

            moon.rightAscension.set(RA, Units::RADIANS);
            moon.declination.set(Decl, Units::RADIANS);
            moon.latitude.set(earthLat, Units::RADIANS);
            moon.longitude.set(earthLon, Units::RADIANS);
            moon.altitude.set(r, Units::METERS);

            // geocentric:
            {
                moon.geocentric = WGS84.geodeticToGeocentric({
                    moon.longitude.as(Units::DEGREES),
                    moon.latitude.as(Units::DEGREES),
                    moon.altitude.as(Units::METERS)
                });
            }

            // ECI:
            {
                double RA = moon.rightAscension.as(Units::RADIANS);
                double DECL = moon.declination.as(Units::RADIANS);
                double R = moon.altitude.as(Units::METERS);
                moon.eci = {
                    R * cos(DECL) * cos(RA),
                    R * cos(DECL) * sin(RA),
                    R * sin(DECL)
                };
            }


            return moon;
        }
    };
}


//------------------------------------------------------------------------


CelestialBody
Ephemeris::sunPosition(const DateTime& dt) const
{
    return Sun().position(dt);
}

CelestialBody
Ephemeris::moonPosition(const DateTime& dt) const
{
    return Moon().position(dt);
}
