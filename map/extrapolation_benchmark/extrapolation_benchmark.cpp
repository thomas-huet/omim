#include "map/extrapolation/extrapolator.hpp"

#include "routing/base/followed_polyline.hpp"

#include "geometry/distance_on_sphere.hpp"
#include "geometry/latlon.hpp"
#include "geometry/mercator.hpp"
#include "geometry/polyline2d.hpp"

#include "platform/location.hpp"

#include "base/logging.hpp"
#include "base/string_utils.hpp"

#include <cmath>
#include <cstdint>
#include <fstream>
#include <limits>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

#include "3party/gflags/src/gflags/gflags_declare.h"
#include "3party/gflags/src/gflags/gflags.h"

// This tool is written to estimate quality of location extrapolation. To launch the benchmark
// you need tracks in csv file with the format described below. To generate the csv file
// you need to go through following steps:
// * take logs from "trafin" project production in gz files
// * extract them (gzip -d)
// * run track_analyzer tool with unmatched_tracks command to generate csv
//   (track_analyzer -cmd unmatched_tracks -in ./trafin_log.20180517-0000)
// * run this tool with csv_path equal to path to the csv
//   (extrapolation_benchmark -csv_path=trafin_log.20180517-0000.track.csv)

DEFINE_string(csv_path, "",
              "Path to csv file with user in following format: mwm id (string), aloha id (string), "
              "latitude of the first coord (double), longitude of the first coord (double), "
              "timestamp in seconds (int), latitude of the second coord (double) and so on.");

using namespace extrapolation;
using namespace location;
using namespace routing;
using namespace std;

namespace
{
struct GpsPoint
{
  GpsPoint(double timestampS, double lat, double lon)
      : m_timestampS(timestampS)
      , m_lat(lat)
      , m_lon(lon)
  {
  }

  double m_timestampS;
  // @TODO(bykoianko) Using LatLog type instead of two double should be considered.
  double m_lat;
  double m_lon;
};

class Expectation
{
public:
  void Add(double value)
  {
    m_averageValue = (m_averageValue * m_counter + value) / (m_counter + 1);
    ++m_counter;
  }

  double Get() const { return m_averageValue; }
  size_t GetCounter() const { return m_counter; }

private:
  double m_averageValue = 0.0;
  size_t m_counter = 0;
};

class ExpectationVec
{
public:
  explicit ExpectationVec(size_t size) { m_mes.resize(size); }

  void Add(vector<double> const & values)
  {
    CHECK_EQUAL(values.size(), m_mes.size(), ());
    for (size_t i = 0; i < values.size(); ++i)
      m_mes[i].Add(values[i]);
  }

  vector<Expectation> const & Get() const { return m_mes; }

private:
  vector<Expectation> m_mes;
};

using Track = vector<GpsPoint>;
using Tracks = vector<Track>;

bool GetString(stringstream & lineStream, string & result)
{
  if (!lineStream.good())
    return false;
  getline(lineStream, result, ',');
  return true;
}

bool GetDouble(stringstream & lineStream, double & result)
{
  string strResult;
  if (!GetString(lineStream, strResult))
    return false;
  return strings::to_double(strResult, result);
}

bool GetUint64(stringstream & lineStream, uint64_t & result)
{
  string strResult;
  if (!GetString(lineStream, strResult))
    return false;
  return strings::to_uint64(strResult, result);
}

bool GetGpsPoint(stringstream & lineStream, uint64_t & timestampS, double & lat, double & lon)
{
  if (!GetUint64(lineStream, timestampS))
    return false;
  if (!GetDouble(lineStream, lat))
    return false;

  return GetDouble(lineStream, lon);
}

/// \brief Fills |tracks| based on file |pathToCsv| content. File |pathToCsv| should be
/// a text file with following lines:
/// <Mwm name (country id)>, <Aloha id>,
/// <Latitude of the first point>, <Longitude of the first point>, <Timestamp in seconds of the first point>,
/// <Latitude of the second point> and so on.
bool Parse(string const & pathToCsv, Tracks & tracks)
{
  tracks.clear();

  std::ifstream csvStream(pathToCsv);
  if (!csvStream.is_open())
    return false;

  while (!csvStream.eof())
  {
    string line;
    getline(csvStream, line);
    stringstream lineStream(line);
    string dummy;
    GetString(lineStream, dummy); // mwm id
    GetString(lineStream, dummy); // aloha id

    Track track;
    while (!lineStream.eof())
    {
      double lat;
      double lon;
      uint64_t timestamp;
      if (!GetGpsPoint(lineStream, timestamp, lat, lon))
        return false;
      track.emplace_back(static_cast<double>(timestamp), lat, lon);
    }
    tracks.push_back(move(track));
  }
  csvStream.close();
  return true;
}

void GpsPointToGpsInfo(GpsPoint const gpsPoint, GpsInfo & gpsInfo)
{
  gpsInfo.m_source = TLocationSource::EAppleNative;
  gpsInfo.m_timestamp = gpsPoint.m_timestampS;
  gpsInfo.m_latitude = gpsPoint.m_lat;
  gpsInfo.m_longitude = gpsPoint.m_lon;
}
} // namespace

/// \brief This benchmark is written to estimate how LinearExtrapolation() extrapolates real users tracks.
/// The idea behind the test is to measure the distance between extrapolated location and real track.
int main(int argc, char * argv[])
{
  google::SetUsageMessage(
      "Location extrapolation benchmark. Calculates expected value and variance for "
      "all extrapolation deviations from tracks passed in csv with with csv_path.");
  google::ParseCommandLineFlags(&argc, &argv, true);

  if (FLAGS_csv_path.empty())
  {
    LOG(LERROR, ("Path to csv file with user tracks should be set."));
    return -1;
  }

  Tracks tracks;
  if (!Parse(FLAGS_csv_path, tracks))
  {
    LOG(LERROR, ("An error while parsing", FLAGS_csv_path, "file. Please check if it has a correct format."));
    return -1;
  }

  // Printing some statistics about tracks.
  size_t trackPointNum = 0;
  for (auto const & t : tracks)
    trackPointNum += t.size();

  double trackLengths = 0;
  for (auto const & t : tracks)
  {
    double trackLenM = 0;
    for (size_t j = 1; j < t.size(); ++j)
    {
      trackLenM += ms::DistanceOnEarth(ms::LatLon(t[j - 1].m_lat, t[j - 1].m_lon),
                                       ms::LatLon(t[j].m_lat, t[j].m_lon));
    }
    trackLengths += trackLenM;
  }

  LOG(LINFO, ("General tracks statistics."
              "\n  Number of tracks:", tracks.size(),
              "\n  Number of track points:", trackPointNum,
              "\n  Average points per track:", trackPointNum / tracks.size(),
              "\n  Average track length:", trackLengths / tracks.size(), "meters"));

  // For all points of each track in |tracks| some extrapolations will be calculated.
  // The number of extrapolations depends on |Extrapolator::kExtrapolationPeriodMs|
  // and |Extrapolator::kMaxExtrapolationTimeMs| and equal for all points.
  // Then expected value and variance each extrapolation will be printed.
  auto const extrapolationNumber = static_cast<size_t>(Extrapolator::kMaxExtrapolationTimeMs /
                                                       Extrapolator::kExtrapolationPeriodMs);
  ExpectationVec mes(extrapolationNumber);
  ExpectationVec squareMes(extrapolationNumber);
  // Number of extrapolations which projections are calculated successfully for.
  size_t projectionCounter = 0;
  for (auto const & t : tracks)
  {
    if (t.size() <= 1)
      continue;

    m2::PolylineD poly;
    for (auto const & p : t)
      poly.Add(MercatorBounds::FromLatLon(p.m_lat, p.m_lon));
    CHECK_EQUAL(poly.GetSize(), t.size(), ());
    FollowedPolyline followedPoly(poly.Begin(), poly.End());
    CHECK(followedPoly.IsValid(), ());

    // For each track point except for the first one some extrapolations will be calculated.
    for (size_t i = 1; i < t.size(); ++i)
    {
      GpsInfo info1;
      GpsPointToGpsInfo(t[i - 1], info1);
      GpsInfo info2;
      GpsPointToGpsInfo(t[i], info2);

      if (!AreCoordsGoodForExtrapolation(info1, info2))
        break;

      vector<double> onePointDeviations;
      vector<double> onePointDeviationsInSqare;
      bool cannotFindClosestProjection = false;
      for (size_t timeMs = Extrapolator::kExtrapolationPeriodMs;
           timeMs <= Extrapolator::kMaxExtrapolationTimeMs;
           timeMs += Extrapolator::kExtrapolationPeriodMs)
      {
        GpsInfo const extrapolated = LinearExtrapolation(info1, info2, timeMs);
        m2::PointD const extrapolatedMerc = MercatorBounds::FromLatLon(extrapolated.m_latitude, extrapolated.m_longitude);

        m2::RectD const posRect = MercatorBounds::MetresToXY(
            extrapolated.m_longitude, extrapolated.m_latitude, 100.0 /* half square in meters */);
        // Note 1. One is deducted from polyline size because in GetClosestProjectionInInterval()
        // is used segment indices but not point indices.
        // Note 2. Calculating projection of the center of |posRect| to polyline segments which
        // are inside of |posRect|.
        auto const & iter = followedPoly.GetClosestProjectionInInterval(
            posRect,
            [&extrapolatedMerc](FollowedPolyline::Iter const & it) {
              return MercatorBounds::DistanceOnEarth(it.m_pt, extrapolatedMerc);
            },
            0 /* start segment index */, followedPoly.GetPolyline().GetSize() - 1);

        if (iter.IsValid())
        {
          ++projectionCounter;
        }
        else
        {
          // This situation is possible if |posRect| param of GetClosestProjectionInInterval()
          // method is too small and there is no segment in |followedPoly|
          // which is covered by this rect. It's a rare situation.
          cannotFindClosestProjection = true;
          break;
        }

        double const distFromPoly = MercatorBounds::DistanceOnEarth(iter.m_pt, extrapolatedMerc);
        onePointDeviations.push_back(distFromPoly);
        onePointDeviationsInSqare.push_back(distFromPoly * distFromPoly);
      }

      if (!cannotFindClosestProjection)
      {
        CHECK_EQUAL(onePointDeviations.size(), extrapolationNumber, ());
        mes.Add(onePointDeviations);
        squareMes.Add(onePointDeviationsInSqare);
      }
    }
  }

  CHECK_GREATER(extrapolationNumber, 0, ());
  LOG(LINFO, ("\n  Processed", mes.Get()[0].GetCounter(), "points.\n",
              "  ", mes.Get()[0].GetCounter() * extrapolationNumber, "extrapolations is calculated.\n",
              "  Projection is calculated for", projectionCounter, "extrapolations."));

  LOG(LINFO, ("Expected value for each extrapolation:"));
  for (size_t i = 0; i < extrapolationNumber; ++i)
  {
    double const variance = squareMes.Get()[i].Get() - mes.Get()[i].Get() * mes.Get()[i].Get();
    LOG(LINFO, ("Extrapolation", i + 1, ",", Extrapolator::kExtrapolationPeriodMs * (i + 1),
                "seconds after point two. Expected value =", mes.Get()[i].Get(),
                "meters.", "Variance =", variance, ". Standard deviation =", sqrt(variance)));
  }

  return 0;
}
