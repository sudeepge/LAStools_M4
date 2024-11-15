/*
===============================================================================

  FILE:  lasinfo.cpp

  CONTENTS:

    This tool reads a LIDAR file in LAS or LAZ format and prints out the info
    from the standard header. It also prints out info from any variable length
    headers and gives detailed info on any geokeys that may be present (this
    can be disabled with the '-no_vrls' flag). The real bounding box of the
    points is computed and compared with the bounding box specified in the header
    (this can be disabled with the '-no_check' flag). It is also possible to
    change or repair some aspects of the header

  PROGRAMMERS:

    info@rapidlasso.de  -  https://rapidlasso.de

  COPYRIGHT:

    (c) 2007-2020, rapidlasso GmbH - fast tools to catch reality

    This is free software; you can redistribute and/or modify it under the
    terms of the GNU Lesser General Licence as published by the Free Software
    Foundation. See the LICENSE.txt file for more information.

    This software is distributed WITHOUT ANY WARRANTY and without even the
    implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.

  CHANGE HISTORY:

    10 June 2021 -- new option '-delete_empty' for deleting LAS files with zero points
    11 November 2020 -- new option '-set_vlr_record_id 2 4711'
    11 November 2020 -- new option '-set_vlr_user_id 1 "hello martin"'
    10 November 2020 -- new option '-set_vlr_description 0 "hello martin"'
    14 October 2017 -- WARN when bounding box miss-matches coordinate resolution
    16 May 2015 -- new option '-set_GUID F794F8A4-A23E-421E-A134-ACF7754E1C54'
     9 July 2012 -- fixed crash that occured when input had a corrupt VLRs
     7 January 2012 -- set bounding box / file source id / point type & size / ...
     6 January 2012 -- make area/density optional
     5 September 2011 -- also compute total area covered and point densities
    26 January 2011 -- added LAStransform because it allows quick previews
    21 January 2011 -- added LASreadOpener and reading of multiple LAS files
     4 January 2011 -- added the LASfilter to drop or keep points
    10 July 2009 -- '-auto_date' sets the day/year from the file creation date
    12 March 2009 -- updated to ask for input if started without arguments
     9 March 2009 -- added output for size of user-defined header data
    17 September 2008 -- updated to deal with LAS format version 1.2
    13 July 2007 -- added the option to "repair" the header and change items
    11 June 2007 -- fixed number of return counts after Vinton found another bug
     6 June 2007 -- added lidardouble2string() after Vinton Valentine's bug report
    25 March 2007 -- sitting at the Pacific Coffee after doing the escalators

===============================================================================
*/

#include "geoprojectionconverter.hpp"
#include "json.hpp"
#include "lasindex.hpp"
#include "lasquadtree.hpp"
#include "lasreader.hpp"
#include "lasutility.hpp"
#include "lasvlrpayload.hpp"
#include "laswriter.hpp"
#include "laszip_decompress_selective_v3.hpp"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#include <windows.h>
#endif
#include "lastool.hpp"

using JsonObject = nlohmann::ordered_json;

static const char* LASpointClassification[32] = {
    "never classified",
    "unclassified",
    "ground",
    "low vegetation",
    "medium vegetation",
    "high vegetation",
    "building",
    "noise",
    "keypoint",
    "water",
    "rail",
    "road surface",
    "overlap",
    "wire guard",
    "wire conductor",
    "tower",
    "wire connector",
    "bridge deck",
    "Reserved for ASPRS Definition",
    "Reserved for ASPRS Definition",
    "Reserved for ASPRS Definition",
    "Reserved for ASPRS Definition",
    "Reserved for ASPRS Definition",
    "Reserved for ASPRS Definition",
    "Reserved for ASPRS Definition",
    "Reserved for ASPRS Definition",
    "Reserved for ASPRS Definition",
    "Reserved for ASPRS Definition",
    "Reserved for ASPRS Definition",
    "Reserved for ASPRS Definition",
    "Reserved for ASPRS Definition",
    "Reserved for ASPRS Definition"};

static inline void VecUpdateMinMax3dv(double min[3], double max[3], const double v[3]) {
  if (v[0] < min[0])
    min[0] = v[0];
  else if (v[0] > max[0])
    max[0] = v[0];
  if (v[1] < min[1])
    min[1] = v[1];
  else if (v[1] > max[1])
    max[1] = v[1];
  if (v[2] < min[2])
    min[2] = v[2];
  else if (v[2] > max[2])
    max[2] = v[2];
}

static inline void VecCopy3dv(double v[3], const double a[3]) {
  v[0] = a[0];
  v[1] = a[1];
  v[2] = a[2];
}

// Function for rounding to a specific number of decimal places
double round_to_decimals(double value, int decimals) {
  double scale = pow(10.0, decimals);  // e.g. 10^10 for 10 decimal places
  return round(value * scale) / scale;
}

// Function for converting formatted character string to double
static double parseFormattedDouble(const char* formattedString) {
  return std::stod(formattedString);
}

static int lidardouble2string(char* string, double value) {
  int len;
  len = sprintf(string, "%.15f", value) - 1;
  while (string[len] == '0') len--;
  if (string[len] != '.') len++;
  string[len] = '\0';
  return len;
}

static I32 lidardouble2string(char* string, double value, double precision) {
  if (precision == 0.1)
    sprintf(string, "%.1f", value);
  else if (precision == 0.01)
    sprintf(string, "%.2f", value);
  else if (precision == 0.001 || precision == 0.002 || precision == 0.005 || precision == 0.025)
    sprintf(string, "%.3f", value);
  else if (precision == 0.0001 || precision == 0.0002 || precision == 0.0005 || precision == 0.0025)
    sprintf(string, "%.4f", value);
  else if (precision == 0.00001 || precision == 0.00002 || precision == 0.00005 || precision == 0.00025)
    sprintf(string, "%.5f", value);
  else if (precision == 0.000001)
    sprintf(string, "%.6f", value);
  else if (precision == 0.0000001)
    sprintf(string, "%.7f", value);
  else if (precision == 0.00000001)
    sprintf(string, "%.8f", value);
  else if (precision == 0.5)
    sprintf(string, "%.1f", value);
  else if (precision == 0.25)
    sprintf(string, "%.2f", value);
  else if (precision == 0.125)
    sprintf(string, "%.3f", value);
  else
    return lidardouble2string(string, value);
  return (I32)strlen(string) - 1;
}

static bool valid_resolution(F64 coordinate, F64 offset, F64 scale_factor) {
  F64 coordinate_without_offset = coordinate - offset;
  F64 fixed_precision_multiplier = coordinate_without_offset / scale_factor;
  I64 quantized_fixed_precision_multiplier = I64_QUANTIZE(fixed_precision_multiplier);
  if ((fabs(fixed_precision_multiplier - quantized_fixed_precision_multiplier)) < 0.001) {
    return true;
  }
  return false;
}

#ifdef COMPILE_WITH_GUI
extern void lasinfo_gui(int argc, char* argv[], LASreadOpener* lasreadopener);
#endif

#ifdef COMPILE_WITH_MULTI_CORE
extern void lasinfo_multi_core(
    int argc, char* argv[], LASreadOpener* lasreadopener, LAShistogram* lashistogram, LASwriteOpener* laswriteopener, int cores, BOOL cpu64);
#endif

class LasTool_lasinfo : public LasTool {
 private:
  bool do_scale_header = false;
  bool header_preread = false;
  bool edit_header = false;
  F64* set_offset = 0;
  F64* set_scale = 0;
  F64* scale_header = 0;

 public:
  void run() {
    int i;
    bool no_header = false;
    bool no_variable_header = false;
    bool no_returns = false;
    bool no_min_max = false;
    bool no_warnings = false;
    bool check_points = true;
    bool compute_density = false;
    bool gps_week = false;
    bool check_outside = true;
    bool report_outside = false;
    bool suppress_z = false;
    bool suppress_classification = false;
    bool suppress_flags = false;
    bool suppress_intensity = false;
    bool suppress_user_data = false;
    bool suppress_point_source = false;
    bool suppress_scan_angle = false;
    bool suppress_RGB = false;
    bool suppress_extra_bytes = false;
    bool repair_bb = false;
    bool repair_counters = false;
    bool delete_empty = false;
    bool json_out = false;
    I32 set_file_source_ID = -1;
    bool set_file_source_ID_from_point_source_ID = false;
    I32 set_global_encoding = -1;
    I64 set_project_ID_GUID_data_1 = -1;
    I32 set_project_ID_GUID_data_2 = -1;
    I32 set_project_ID_GUID_data_3 = -1;
    U8 set_project_ID_GUID_data_4[8];
    I8 set_version_major = -1;
    I8 set_version_minor = -1;
    I8* set_system_identifier = 0;
    I8* set_generating_software = 0;
    I32 set_creation_day = -1;
    I32 set_creation_year = -1;
    I32 set_vlr_user_id_index = -1;
    const CHAR* set_vlr_user_id = 0;
    I32 set_vlr_record_id_index = -1;
    I32 set_vlr_record_id = 0;
    I32 set_vlr_description_index = -1;
    const CHAR* set_vlr_description = 0;
    //  I32 set_evlr_description_index = -1;
    //  const CHAR* set_evlr_description = 0;
    U16 set_header_size = 0;
    U32 set_offset_to_point_data = 0;
    I32 set_number_of_variable_length_records = -1;
    I32 set_point_data_format = -1;
    I32 set_point_data_record_length = -1;
    I32 set_number_of_point_records = -1;
    I32 set_number_of_points_by_return[5] = {-1, -1, -1, -1, -1};
    F64* set_bounding_box = 0;
    I64 set_start_of_waveform_data_packet_record = -1;
    I32 set_geotiff_epsg = -1;
    bool auto_date_creation = false;
    FILE* file_out = stderr;
    U32 horizontal_units = 0;
    // extract a subsequence
    I64 subsequence_start = 0;
    I64 subsequence_stop = I64_MAX;
    U32 progress = 0;
    // rename
    CHAR* base_name = 0;
    JsonObject json_main;

    LAShistogram lashistogram;
    LASreadOpener lasreadopener;
    GeoProjectionConverter geoprojectionconverter;
    LASwriteOpener laswriteopener;

    lasreadopener.set_keep_copc(TRUE);

    if (argc == 1) {
#ifdef COMPILE_WITH_GUI
      lasinfo_gui(argc, argv, 0);
#else
      wait_on_exit = true;
      fprintf(stderr, "%s is better run in the command line\n", argv[0]);
      char file_name[256];
      fprintf(stderr, "enter input file: ");
      fgets(file_name, 256, stdin);
      file_name[strlen(file_name) - 1] = '\0';
      lasreadopener.set_file_name(file_name);
#endif
    } else {
      for (i = 1; i < argc; i++) {
        if ((unsigned char)argv[i][0] == 0x96) argv[i][0] = '-';
      }
      if (!lashistogram.parse(argc, argv)) byebye();
      lasreadopener.parse(argc, argv);
      geoprojectionconverter.parse(argc, argv);
      laswriteopener.parse(argc, argv);
    }

    if (laswriteopener.is_piped()) {
      file_out = stdout;
    }

    auto arg_local = [&](int& i) -> bool {
      if (strcmp(argv[i], "-quiet") == 0) {
        file_out = 0;
      } else if (strcmp(argv[i], "-otxt") == 0) {
        laswriteopener.set_appendix("_info");
        laswriteopener.set_format("txt");
      } else if (strcmp(argv[i], "-ojs") == 0) {
        laswriteopener.set_appendix("_info");
        laswriteopener.set_format("json");
      } else if (strcmp(argv[i], "-nh") == 0 || strcmp(argv[i], "-no_header") == 0) {
        no_header = true;
      } else if (strcmp(argv[i], "-nv") == 0 || strcmp(argv[i], "-no_vlrs") == 0) {
        no_variable_header = true;
      } else if (strcmp(argv[i], "-nr") == 0 || strcmp(argv[i], "-no_returns") == 0) {
        no_returns = true;
      } else if (strcmp(argv[i], "-nmm") == 0 || strcmp(argv[i], "-no_min_max") == 0) {
        no_min_max = true;
      } else if (strcmp(argv[i], "-nw") == 0 || strcmp(argv[i], "-no_warnings") == 0) {
        no_warnings = true;
      } else if (strcmp(argv[i], "-nc") == 0 || strcmp(argv[i], "-no_check") == 0) {
        check_points = false;
      } else if (strcmp(argv[i], "-cd") == 0 || strcmp(argv[i], "-compute_density") == 0) {
        compute_density = true;
      } else if (strcmp(argv[i], "-gw") == 0 || strcmp(argv[i], "-gps_week") == 0) {
        gps_week = true;
      } else if (strcmp(argv[i], "-nco") == 0 || strcmp(argv[i], "-no_check_outside") == 0) {
        check_outside = false;
      } else if (strcmp(argv[i], "-js") == 0 || strcmp(argv[i], "-json") == 0) {
        json_out = true;
      } else if (strcmp(argv[i], "-ro") == 0 || strcmp(argv[i], "-report_outside") == 0) {
        report_outside = true;
        check_outside = true;
      } else if (strcmp(argv[i], "-subseq") == 0) {
        if ((i + 2) >= argc) {
          laserror("'%s' needs 2 arguments: start stop", argv[i]);
        }
        if (sscanf_las(argv[i + 1], "%lld", &subsequence_start) != 1) {
          laserror("'%s' needs 2 arguments: start stop but '%s' is not a valid start", argv[i], argv[i + 1]);
        }
        if (subsequence_start < 0) {
          laserror("'%s' needs 2 arguments: start stop but '%lld' is not a valid start", argv[i], subsequence_start);
        }
        if (sscanf_las(argv[i + 2], "%lld", &subsequence_stop) != 1) {
          laserror("'%s' needs 2 arguments: start stop but '%s' is not a valid stop", argv[i], argv[i + 2]);
        }
        if (subsequence_stop < 0) {
          laserror("'%s' needs 2 arguments: start stop but '%lld' is not a valid stop", argv[i], subsequence_stop);
        }
        if (subsequence_start >= subsequence_stop) {
          laserror(
              "'%s' needs 2 arguments: start stop but '%lld' and '%lld' are no valid start and stop combination ", argv[i], subsequence_start,
              subsequence_stop);
        }
        i += 2;
      } else if (strcmp(argv[i], "-start_at_point") == 0) {
        if ((i + 1) >= argc) {
          laserror("'%s' needs 1 argument: start", argv[i]);
        }
        if (sscanf_las(argv[i + 1], "%lld", &subsequence_start) != 1) {
          laserror("'%s' needs 1 argument: start but '%s' is not a valid start", argv[i], argv[i + 1]);
        }
        if (subsequence_start < 0) {
          laserror("'%s' needs 1 argument: start but '%lld' is not a valid start", argv[i], subsequence_start);
        }
        i += 1;
      } else if (strcmp(argv[i], "-stop_at_point") == 0) {
        if ((i + 1) >= argc) {
          laserror("'%s' needs 1 argument: stop", argv[i]);
        }
        if (sscanf_las(argv[i + 1], "%lld", &subsequence_stop) != 1) {
          laserror("'%s' needs 1 argument: start but '%s' is not a valid stop", argv[i], argv[i + 1]);
        }
        if (subsequence_stop < 0) {
          laserror("'%s' needs 1 argument: start but '%lld' is not a valid stop", argv[i], subsequence_stop);
        }
        i += 1;
      } else if (strncmp(argv[i], "-repair", 7) == 0) {
        if (strcmp(argv[i], "-repair") == 0) {
          repair_bb = true;
          repair_counters = true;
        } else if (strcmp(argv[i], "-repair_bb") == 0) {
          repair_bb = true;
        } else if (strcmp(argv[i], "-repair_counters") == 0) {
          repair_counters = true;
        }
      } else if (strcmp(argv[i], "-delete_empty") == 0) {
        delete_empty = true;
      } else if (strcmp(argv[i], "-auto_date") == 0 || strcmp(argv[i], "-auto_creation_date") == 0 || strcmp(argv[i], "-auto_creation") == 0) {
        auto_date_creation = true;
      } else if (strncmp(argv[i], "-set_", 5) == 0) {
        if (strcmp(argv[i], "-set_file_source_ID") == 0) {
          if ((i + 1) >= argc) {
            laserror("'%s' needs 1 argument: index", argv[i]);
          }
          if (sscanf_las(argv[i + 1], "%u", &set_file_source_ID) != 1) {
            laserror("'%s' needs 1 argument: index but '%s' is no valid index", argv[i], argv[i + 1]);
          }
          if (set_file_source_ID > U16_MAX) {
            laserror("'%s' needs 1 argument: index between 0 and %u but %u is out of range", argv[i], U16_MAX, set_file_source_ID);
          }
          i++;
          edit_header = true;
        } else if (strcmp(argv[i], "-set_file_source_ID_from_point_source_ID") == 0) {
          set_file_source_ID_from_point_source_ID = true;
          edit_header = true;
        } else if (strcmp(argv[i], "-set_GUID") == 0) {
          if ((i + 1) >= argc) {
            laserror("'%s' needs 1 argument: value1", argv[i]);
          }
          i++;
          if (sscanf_las(
                  argv[i], "%I64x-%x-%x-%02X%02X-%02X%02X%02X%02X%02X%02X", &set_project_ID_GUID_data_1, &set_project_ID_GUID_data_2,
                  &set_project_ID_GUID_data_3, &set_project_ID_GUID_data_4[0], &set_project_ID_GUID_data_4[1], &set_project_ID_GUID_data_4[2],
                  &set_project_ID_GUID_data_4[3], &set_project_ID_GUID_data_4[4], &set_project_ID_GUID_data_4[5], &set_project_ID_GUID_data_4[6],
                  &set_project_ID_GUID_data_4[7]) != 11) {
            if ((i + 1) >= argc) {
              laserror("'%s' needs hexadecimal GUID in 'F794F8A4-A23E-421E-A134-ACF7754E1C54' format", argv[i]);
            }
          }
          edit_header = true;
        } else if (strcmp(argv[i], "-set_system_identifier") == 0) {
          if ((i + 1) >= argc) {
            laserror("'%s' needs 1 argument: name", argv[i]);
          }
          i++;
          set_system_identifier = new I8[32];
          memset(set_system_identifier, 0, 32);
          strncpy_las(set_system_identifier, 32, argv[i], 32);
          edit_header = true;
        } else if (strcmp(argv[i], "-set_generating_software") == 0) {
          if ((i + 1) >= argc) {
            laserror("'%s' needs 1 argument: name", argv[i]);
          }
          i++;
          set_generating_software = new I8[32];
          memset(set_generating_software, 0, 32);
          strncpy_las(set_generating_software, 32, argv[i], 32);
          edit_header = true;
        } else if (strcmp(argv[i], "-set_bb") == 0 || strcmp(argv[i], "-set_bounding_box") == 0) {
          if ((i + 6) >= argc) {
            laserror("'%s' needs 6 arguments: min_x min_y min_z max_x max_y max_z", argv[i]);
          }
          set_bounding_box = new F64[6];
          i++;
          set_bounding_box[1] = atof(argv[i]);  // x min
          i++;
          set_bounding_box[3] = atof(argv[i]);  // y min
          i++;
          set_bounding_box[5] = atof(argv[i]);  // z min
          i++;
          set_bounding_box[0] = atof(argv[i]);  // x max
          i++;
          set_bounding_box[2] = atof(argv[i]);  // y max
          i++;
          set_bounding_box[4] = atof(argv[i]);  // z max
          edit_header = true;
        } else if (strcmp(argv[i], "-set_offset") == 0) {
          if ((i + 3) >= argc) {
            laserror("'%s' needs 3 arguments: x y z", argv[i]);
          }
          set_offset = new F64[3];
          i++;
          set_offset[0] = atof(argv[i]);
          i++;
          set_offset[1] = atof(argv[i]);
          i++;
          set_offset[2] = atof(argv[i]);
          edit_header = true;
        } else if (strcmp(argv[i], "-set_scale") == 0) {
          if ((i + 1) >= argc) {
            laserror("'%s' needs 1 or 3 arguments: scale (xyz or x y z)", argv[i]);
          }
          set_scale = new F64[3];
          i++;
          set_scale[0] = atof(argv[i]);
          if ((i + 2) < argc) {
            i++;
            set_scale[1] = atof(argv[i]);
            i++;
            set_scale[2] = atof(argv[i]);
          } else {
            set_scale[1] = set_scale[0];
            set_scale[2] = set_scale[0];
          }
          edit_header = true;
        } else if (strcmp(argv[i], "-set_global_encoding") == 0) {
          if ((i + 1) >= argc) {
            laserror("'%s' needs 1 argument: number", argv[i]);
          }
          i++;
          set_global_encoding = atoi(argv[i]);
          edit_header = true;
        } else if (strcmp(argv[i], "-set_version") == 0) {
          if ((i + 1) >= argc) {
            laserror("'%s' needs 1 argument: major.minor", argv[i]);
          }
          i++;
          int major;
          int minor;
          if (sscanf_las(argv[i], "%d.%d", &major, &minor) != 2) {
            laserror("cannot understand argument '%s' of '%s'", argv[i], argv[i - 1]);
          }
          set_version_major = (I8)major;
          set_version_minor = (I8)minor;
          edit_header = true;
        } else if (strcmp(argv[i], "-set_creation_date") == 0 || strcmp(argv[i], "-set_file_creation") == 0) {
          if ((i + 2) >= argc) {
            laserror("'%s' needs 2 arguments: day year", argv[i]);
          }
          i++;
          set_creation_day = (U16)atoi(argv[i]);
          i++;
          set_creation_year = (U16)atoi(argv[i]);
          edit_header = true;
        } else if (strcmp(argv[i], "-set_number_of_point_records") == 0) {
          if ((i + 1) >= argc) {
            laserror("'%s' needs 1 argument: number", argv[i]);
          }
          i++;
          set_number_of_point_records = atoi(argv[i]);
          edit_header = true;
        } else if (strcmp(argv[i], "-set_number_of_points_by_return") == 0) {
          if ((i + 5) >= argc) {
            laserror("'%s' needs 5 arguments: ret1 ret2 ret3 ret4 ret5", argv[i]);
          }
          i++;
          set_number_of_points_by_return[0] = atoi(argv[i]);
          i++;
          set_number_of_points_by_return[1] = atoi(argv[i]);
          i++;
          set_number_of_points_by_return[2] = atoi(argv[i]);
          i++;
          set_number_of_points_by_return[3] = atoi(argv[i]);
          i++;
          set_number_of_points_by_return[4] = atoi(argv[i]);
          edit_header = true;
        } else if (strcmp(argv[i], "-set_header_size") == 0) {
          if ((i + 1) >= argc) {
            laserror("'%s' needs 1 argument: size", argv[i]);
          }
          i++;
          set_header_size = atoi(argv[i]);
          edit_header = true;
        } else if (strcmp(argv[i], "-set_offset_to_point_data") == 0) {
          if ((i + 1) >= argc) {
            laserror("'%s' needs 1 argument: offset", argv[i]);
          }
          i++;
          set_offset_to_point_data = atoi(argv[i]);
          edit_header = true;
        } else if (strcmp(argv[i], "-set_number_of_variable_length_records") == 0) {
          if ((i + 1) >= argc) {
            laserror("'%s' needs 1 argument: number", argv[i]);
          }
          i++;
          set_number_of_variable_length_records = atoi(argv[i]);
          edit_header = true;
        } else if (strcmp(argv[i], "-set_point_data_format") == 0) {
          if ((i + 1) >= argc) {
            laserror("'%s' needs 1 argument: type", argv[i]);
          }
          i++;
          set_point_data_format = atoi(argv[i]);
          edit_header = true;
        } else if (strcmp(argv[i], "-set_point_data_record_length") == 0) {
          if ((i + 1) >= argc) {
            laserror("'%s' needs 1 argument: size", argv[i]);
          }
          i++;
          set_point_data_record_length = atoi(argv[i]);
          edit_header = true;
        } else if (strcmp(argv[i], "-set_start_of_waveform_data_packet_record") == 0) {
          if ((i + 1) >= argc) {
            laserror("'%s' needs 1 argument: start", argv[i]);
          }
          i++;
          set_start_of_waveform_data_packet_record = atoi(argv[i]);
          edit_header = true;
        } else if (strcmp(argv[i], "-set_vlr_user_id") == 0) {
          if ((i + 2) >= argc) {
            laserror("'%s' needs 2 arguments: index user_id", argv[i]);
          }
          if (sscanf_las(argv[i + 1], "%d", &set_vlr_user_id_index) != 1) {
            laserror("'%s' needs 2 arguments: index user_ID but '%s' is no valid index", argv[i], argv[i + 1]);
          }
          if ((set_vlr_user_id_index < 0) || (set_vlr_user_id_index > U16_MAX)) {
            laserror("'%s' needs 2 arguments: index user_ID, but index %d is out of range", argv[i], set_vlr_user_id_index);
          }
          i++;
          i++;
          set_vlr_user_id = argv[i];
          edit_header = true;
        } else if (strcmp(argv[i], "-set_vlr_record_id") == 0) {
          if ((i + 2) >= argc) {
            laserror("'%s' needs 2 arguments: index record_ID", argv[i]);
          }
          if (sscanf_las(argv[i + 1], "%d", &set_vlr_record_id_index) != 1) {
            laserror("'%s' needs 2 arguments: index record_ID but '%s' is no valid index", argv[i], argv[i + 1]);
          }
          if ((set_vlr_record_id_index < 0) || (set_vlr_record_id_index > U16_MAX)) {
            laserror("'%s' needs 2 arguments: index record_ID, but index %d is out of range", argv[i], set_vlr_record_id_index);
          }
          if (sscanf_las(argv[i + 2], "%d", &set_vlr_record_id) != 1) {
            laserror("'%s' needs 2 arguments: index record_ID but '%s' is no valid record ID", argv[i], argv[i + 2]);
          }
          if ((set_vlr_record_id < 0) || (set_vlr_record_id > U16_MAX)) {
            laserror("'%s' needs 2 arguments: index record_ID, but record_ID %d is out of range", argv[i], set_vlr_record_id_index);
          }
          i++;
          i++;
          edit_header = true;
        } else if (strcmp(argv[i], "-set_vlr_description") == 0) {
          if ((i + 2) >= argc) {
            laserror("'%s' needs 2 arguments: index description", argv[i]);
          }
          if (sscanf_las(argv[i + 1], "%d", &set_vlr_description_index) != 1) {
            laserror("'%s' needs 2 arguments: index description but '%s' is no valid index", argv[i], argv[i + 1]);
          }
          if ((set_vlr_description_index < 0) || (set_vlr_description_index > U16_MAX)) {
            laserror("'%s' needs 2 arguments: index description, but index %d is out of range", argv[i], set_vlr_description_index);
          }
          i++;
          i++;
          set_vlr_description = argv[i];
          edit_header = true;
        }
        /*
              else if (strcmp(argv[i],"~set_evlr_description") == 0)
              {
                if ((i+2) >= argc)
                {
                  laserror("'%s' needs 2 arguments: index description", argv[i]);
                }
                if (sscanf(argv[i+1], "%d", &set_evlr_description_index) != 1)
                {
                  laserror("'%s' needs 2 arguments: index description but '%s' is no valid index", argv[i], argv[i+1]);
                }
                if ((set_evlr_description_index < 0) || (set_evlr_description_index > U16_MAX))
                {
                  laserror("'%s' needs 2 arguments: index description, but index %d is out of range", argv[i], set_vlr_description_index);
                }
                      i++;
                      i++;
                set_evlr_description = argv[i];
                edit_header = true;
                  }
        */
        else if (strcmp(argv[i], "-set_geotiff_epsg") == 0) {
          if ((i + 1) >= argc) {
            laserror("'%s' needs 1 argument: code", argv[i]);
          }
          if (sscanf_las(argv[i + 1], "%u", &set_geotiff_epsg) != 1) {
            laserror("'%s' needs 1 argument: code but '%s' is no valid code", argv[i], argv[i + 1]);
          }
          if (set_geotiff_epsg > U16_MAX) {
            laserror("'%s' needs 1 argument: code between 0 and %u but %u is out of range", argv[i], U16_MAX, set_geotiff_epsg);
          }
          i++;
          edit_header = true;
        } else {
          laserror("cannot understand argument '%s'", argv[i]);
        }
      } else if (strcmp(argv[i], "-scale_header") == 0) {
        if ((i + 1) >= argc) {
          laserror("'%s' needs 1 or 3 arguments: header scale factor (factor or fx fy fz)", argv[i]);
        }
        scale_header = new F64[3];
        i++;
        scale_header[0] = atof(argv[i]);
        if ((i + 2) < argc) {
          i++;
          scale_header[1] = atof(argv[i]);
          i++;
          scale_header[2] = atof(argv[i]);
        } else {
          scale_header[1] = scale_header[0];
          scale_header[2] = scale_header[0];
        }
        edit_header = true;
        do_scale_header = true;
        header_preread = true;
      } else if (strncmp(argv[i], "-suppress_", 10) == 0) {
        if (strcmp(argv[i], "-suppress_z") == 0) {
          suppress_z = true;
        } else if (strcmp(argv[i], "-suppress_classification") == 0) {
          suppress_classification = true;
        } else if (strcmp(argv[i], "-suppress_flags") == 0) {
          suppress_flags = true;
        } else if (strcmp(argv[i], "-suppress_intensity") == 0) {
          suppress_intensity = true;
        } else if (strcmp(argv[i], "-suppress_user_data") == 0) {
          suppress_user_data = true;
        } else if (strcmp(argv[i], "-suppress_point_source") == 0) {
          suppress_point_source = true;
        } else if (strcmp(argv[i], "-suppress_scan_angle") == 0) {
          suppress_scan_angle = true;
        } else if (strcmp(argv[i], "-suppress_RGB") == 0) {
          suppress_RGB = true;
        } else if (strcmp(argv[i], "-suppress_extra_bytes") == 0) {
          suppress_extra_bytes = true;
        } else {
          laserror("cannot understand argument '%s'", argv[i]);
        }
      } else if (strcmp(argv[i], "-rename") == 0) {
        if ((i + 1) >= argc) {
          laserror("'%s' needs 1 argument: base name", argv[i]);
        }
        i++;
        base_name = argv[i];
      } else if (strcmp(argv[i], "-progress") == 0) {
        if ((i + 1) >= argc) {
          laserror("'%s' needs 1 argument: every", argv[i]);
        }
        if (sscanf_las(argv[i + 1], "%u", &progress) != 1) {
          laserror("'%s' needs 1 argument: every but '%s' is no valid number", argv[i], argv[i + 1]);
        }
        if (progress == 0) {
          laserror("'%s' needs 1 argument: every but '%u' is no valid number", argv[i], progress);
        }
        i++;
      } else if ((argv[i][0] != '-') && (lasreadopener.get_file_name_number() == 0)) {
        lasreadopener.add_file_name(argv[i]);
        argv[i][0] = '\0';
      } else {
        return false;
      }
      return true;
    };

    parse(arg_local);

#ifdef COMPILE_WITH_GUI
    if (gui) {
      lasinfo_gui(argc, argv, &lasreadopener);
    }
#endif

#ifdef COMPILE_WITH_MULTI_CORE
    if (cores > 1) {
      if (lasreadopener.get_file_name_number() < 2) {
        LASMessage(LAS_WARNING, "only %u input files. ignoring '-cores %d' ...", lasreadopener.get_file_name_number(), cores);
      } else if (lasreadopener.is_merged()) {
        LASMessage(LAS_WARNING, "input files merged on-the-fly. ignoring '-cores %d' ...", cores);
      } else {
        lasinfo_multi_core(argc, argv, &lasreadopener, &lashistogram, &laswriteopener, cores, cpu64);
      }
    }
    if (cpu64) {
      lasinfo_multi_core(argc, argv, &lasreadopener, &lashistogram, &laswriteopener, 1, TRUE);
    }
#endif

    // check input

    if (!lasreadopener.active()) {
      laserror("no input specified");
    }

    // omit "suppressed" layers from LAZ decompression (for new LAS 1.4 point types only)

    U32 decompress_selective = LASZIP_DECOMPRESS_SELECTIVE_ALL;

    if (suppress_z) {
      decompress_selective &= ~LASZIP_DECOMPRESS_SELECTIVE_Z;
    }

    if (suppress_classification) {
      decompress_selective &= ~LASZIP_DECOMPRESS_SELECTIVE_CLASSIFICATION;
    }

    if (suppress_flags) {
      decompress_selective &= ~LASZIP_DECOMPRESS_SELECTIVE_FLAGS;
    }

    if (suppress_intensity) {
      decompress_selective &= ~LASZIP_DECOMPRESS_SELECTIVE_INTENSITY;
    }

    if (suppress_user_data) {
      decompress_selective &= ~LASZIP_DECOMPRESS_SELECTIVE_USER_DATA;
    }

    if (suppress_point_source) {
      decompress_selective &= ~LASZIP_DECOMPRESS_SELECTIVE_POINT_SOURCE;
    }

    if (suppress_scan_angle) {
      decompress_selective &= ~LASZIP_DECOMPRESS_SELECTIVE_SCAN_ANGLE;
    }

    if (suppress_RGB) {
      decompress_selective &= ~LASZIP_DECOMPRESS_SELECTIVE_RGB;
    }

    if (suppress_extra_bytes) {
      decompress_selective &= ~LASZIP_DECOMPRESS_SELECTIVE_EXTRA_BYTES;
    }

    lasreadopener.set_decompress_selective(decompress_selective);

    // possibly loop over multiple input files
    while (lasreadopener.active()) {
      LASreader* lasreader = nullptr;
      LASheader* lasheader = nullptr;
      if (edit_header) {
        if (lasreadopener.is_piped()) {
          laserror("cannot edit header of piped input");
          edit_header = false;
        } else if (lasreadopener.is_merged()) {
          laserror("cannot edit header of merged input");
          edit_header = false;
        } else if (lasreadopener.is_buffered()) {
          laserror("cannot edit header of buffered input");
          edit_header = false;
        }
        const CHAR* file_name = lasreadopener.get_file_name(lasreadopener.get_file_name_current());
        if ((strstr(file_name, ".laz") == 0) && (strstr(file_name, ".las") == 0) && (strstr(file_name, ".LAZ") == 0) &&
            (strstr(file_name, ".LAS") == 0)) {
          laserror("can only edit for LAS or LAZ files, not for '%s'", file_name);
          edit_header = false;
        }
        if (set_file_source_ID_from_point_source_ID) {
          LASreader* lasreader = lasreadopener.open(file_name, FALSE);
          if (lasreader == 0) {
            laserror("cannot open lasreader for '%s'", file_name);
          }
          if (lasreader->read_point()) {
            set_file_source_ID = lasreader->point.get_point_source_ID();
          } else {
            set_file_source_ID = -1;
          }
          lasreader->close();
          delete lasreader;
        }
        I64 set_vlr_user_id_pos = -1;
        if (set_vlr_user_id_index != -1) {
          LASreader* lasreader = lasreadopener.open(file_name, FALSE);
          if (lasreader == 0) {
            laserror("cannot open lasreader for '%s'", file_name);
          }
          if (set_vlr_user_id_index < (I32)lasreader->header.number_of_variable_length_records) {
            I64 pos = lasreader->header.header_size;
            for (i = 0; i < (int)set_vlr_user_id_index; i++) {
              pos += 54;
              pos += lasreader->header.vlrs[i].record_length_after_header;
            }
            set_vlr_user_id_pos = pos + 2;
          } else {
            LASMessage(LAS_INFO, "SKIPPING: cannot set user_ID of VLR with index %d for file '%s'", set_vlr_user_id_index, file_name);
          }
          lasreader->close();
          delete lasreader;
        }
        I64 set_vlr_record_id_pos = -1;
        if (set_vlr_record_id_index != -1) {
          LASreader* lasreader = lasreadopener.open(file_name, FALSE);
          if (lasreader == 0) {
            laserror("cannot open lasreader for '%s'", file_name);
          }
          if (set_vlr_record_id_index < (I32)lasreader->header.number_of_variable_length_records) {
            I64 pos = lasreader->header.header_size;
            for (i = 0; i < (int)set_vlr_record_id_index; i++) {
              pos += 54;
              pos += lasreader->header.vlrs[i].record_length_after_header;
            }
            set_vlr_record_id_pos = pos + 18;
          } else {
            LASMessage(LAS_INFO, "SKIPPING: cannot set record_ID of VLR with index %d for file '%s'", set_vlr_record_id_index, file_name);
          }
          lasreader->close();
          delete lasreader;
        }
        I64 set_vlr_description_pos = -1;
        if (set_vlr_description_index != -1) {
          LASreader* lasreader = lasreadopener.open(file_name, FALSE);
          if (lasreader == 0) {
            laserror("cannot open lasreader for '%s'", file_name);
          }
          if (set_vlr_description_index < (I32)lasreader->header.number_of_variable_length_records) {
            I64 pos = lasreader->header.header_size;
            for (i = 0; i < (int)set_vlr_description_index; i++) {
              pos += 54;
              pos += lasreader->header.vlrs[i].record_length_after_header;
            }
            set_vlr_description_pos = pos + 22;
          } else {
            LASMessage(LAS_INFO, "SKIPPING: cannot set desciption of VLR with index %d for file '%s'", set_vlr_description_index, file_name);
          }
          lasreader->close();
          delete lasreader;
        }
        I64 set_geotiff_vlr_geo_keys_pos = -1;
        U32 set_geotiff_vlr_geo_keys_length = 0;
        I64 set_geotiff_vlr_geo_double_pos = -1;
        U32 set_geotiff_vlr_geo_double_length = 0;
        I64 set_geotiff_vlr_geo_ascii_pos = -1;
        U32 set_geotiff_vlr_geo_ascii_length = 0;
        if (set_geotiff_epsg != -1) {
          LASreader* lasreader = lasreadopener.open(file_name, FALSE);
          if (lasreader == 0) {
            laserror("cannot open lasreader for '%s'", file_name);
          }
          I64 pos = lasreader->header.header_size;
          for (i = 0; i < (int)lasreader->header.number_of_variable_length_records; i++) {
            pos += 54;
            if (strcmp(lasreader->header.vlrs[i].user_id, "LASF_Projection") == 0) {
              if (lasreader->header.vlrs[i].record_id == 34735)  // GeoKeyDirectoryTag
              {
                set_geotiff_vlr_geo_keys_pos = pos;
                set_geotiff_vlr_geo_keys_length = lasreader->header.vlrs[i].record_length_after_header;
              } else if (lasreader->header.vlrs[i].record_id == 34736)  // GeoDoubleParamsTag
              {
                set_geotiff_vlr_geo_double_pos = pos;
                set_geotiff_vlr_geo_double_length = lasreader->header.vlrs[i].record_length_after_header;
              } else if (lasreader->header.vlrs[i].record_id == 34737)  // GeoAsciiParamsTag
              {
                set_geotiff_vlr_geo_ascii_pos = pos;
                set_geotiff_vlr_geo_ascii_length = lasreader->header.vlrs[i].record_length_after_header;
              }
            }
            pos += lasreader->header.vlrs[i].record_length_after_header;
          }
          lasreader->close();
          delete lasreader;
        }
        FILE* file = LASfopen(file_name, "rb+");
        if (file == 0) {
          laserror("could not open file '%s' for edit of header", file_name);
          edit_header = false;
        } else if (edit_header) {
          // preread header actions
          if (header_preread) {
            LASreader* lasreader = lasreadopener.open(file_name, FALSE);
            if (lasreader == 0) {
              laserror("cannot open lasreader for '%s'", file_name);
            }
            if (do_scale_header) {
              if (set_scale) {
                laserror("invalid combination of -set_scale and -scale_header");
              }
              if (set_offset) {
                laserror("invalid combination of -set_offset and -scale_header");
              }
              if (set_bounding_box) {
                laserror("invalid combination of -set_bounding_box and -scale_header");
              }
              set_scale = new F64[3];
              set_scale[0] = lasreader->header.x_scale_factor * scale_header[0];
              set_scale[1] = lasreader->header.y_scale_factor * scale_header[1];
              set_scale[2] = lasreader->header.z_scale_factor * scale_header[2];
              //
              set_offset = new F64[3];
              set_offset[0] = lasreader->header.x_offset * scale_header[0];
              set_offset[1] = lasreader->header.y_offset * scale_header[1];
              set_offset[2] = lasreader->header.z_offset * scale_header[2];
              // clang-format off
                            LASMessage(LAS_VERBOSE, "set offset from [%f/%f/%f] to [%f/%f/%f]", 
                              lasreader->header.x_offset, lasreader->header.y_offset, lasreader->header.z_offset, 
                              set_offset[0], set_offset[1], set_offset[2]);
              // clang-format on
              set_bounding_box = new F64[6];  // x2,x1,y2,y1,z2,z1
              set_bounding_box[1] = lasreader->header.min_x * scale_header[0];
              set_bounding_box[3] = lasreader->header.min_y * scale_header[1];
              set_bounding_box[5] = lasreader->header.min_z * scale_header[2];
              set_bounding_box[0] = lasreader->header.max_x * scale_header[0];
              set_bounding_box[2] = lasreader->header.max_y * scale_header[1];
              set_bounding_box[4] = lasreader->header.max_z * scale_header[2];
              // clang-format off
                            LASMessage(LAS_VERBOSE, "set bounding box from [%f/%f/%f-%f/%f/%f] to [%f/%f/%f-%f/%f/%f]", 
                              lasreader->header.min_x, lasreader->header.min_y, lasreader->header.min_z, 
                              lasreader->header.max_x, lasreader->header.max_y, lasreader->header.max_z, 
                              set_bounding_box[1], set_bounding_box[3], set_bounding_box[5], 
                              set_bounding_box[0], set_bounding_box[2], set_bounding_box[4]);
              // clang-format on
            }
            lasreader->close();
            delete lasreader;
          }

          if (set_file_source_ID != -1) {
            U16 file_source_ID = U16_CLAMP(set_file_source_ID);
            fseek(file, 4, SEEK_SET);
            fwrite(&file_source_ID, sizeof(U16), 1, file);
          }
          if (set_global_encoding != -1) {
            U16 global_encoding = U16_CLAMP(set_global_encoding);
            fseek(file, 6, SEEK_SET);
            fwrite(&global_encoding, sizeof(U16), 1, file);
          }
          if (set_project_ID_GUID_data_1 != -1) {
            fseek(file, 8, SEEK_SET);
            U32 GUID_data_1 = U32_CLAMP(set_project_ID_GUID_data_1);
            U16 GUID_data_2 = U16_CLAMP(set_project_ID_GUID_data_2);
            U16 GUID_data_3 = U16_CLAMP(set_project_ID_GUID_data_3);
            fwrite(&GUID_data_1, sizeof(U32), 1, file);
            fwrite(&GUID_data_2, sizeof(U16), 1, file);
            fwrite(&GUID_data_3, sizeof(U16), 1, file);
            fwrite(&set_project_ID_GUID_data_4[0], 8, 1, file);
          }
          if (set_version_major != -1) {
            fseek(file, 24, SEEK_SET);
            fwrite(&set_version_major, sizeof(I8), 1, file);
          }
          if (set_version_minor != -1) {
            fseek(file, 25, SEEK_SET);
            fwrite(&set_version_minor, sizeof(I8), 1, file);
          }
          if (set_system_identifier) {
            fseek(file, 26, SEEK_SET);
            fwrite(set_system_identifier, sizeof(I8), 32, file);
          }
          if (set_generating_software) {
            fseek(file, 58, SEEK_SET);
            fwrite(set_generating_software, sizeof(I8), 32, file);
          }
          if (set_creation_day != -1) {
            U16 creation_day = U16_CLAMP(set_creation_day);
            fseek(file, 90, SEEK_SET);
            fwrite(&creation_day, sizeof(U16), 1, file);
          }
          if (set_creation_year != -1) {
            U16 creation_year = U16_CLAMP(set_creation_year);
            fseek(file, 92, SEEK_SET);
            fwrite(&creation_year, sizeof(U16), 1, file);
          }
          if (set_header_size) {
            fseek(file, 94, SEEK_SET);
            fwrite(&set_header_size, sizeof(U16), 1, file);
          }
          if (set_offset_to_point_data) {
            fseek(file, 96, SEEK_SET);
            fwrite(&set_offset_to_point_data, sizeof(U32), 1, file);
          }
          if (set_number_of_variable_length_records != -1) {
            fseek(file, 100, SEEK_SET);
            fwrite(&set_number_of_variable_length_records, sizeof(U32), 1, file);
          }
          if (set_point_data_format != -1) {
            U8 point_data_format = U8_CLAMP(set_point_data_format);
            fseek(file, 104, SEEK_SET);
            fwrite(&point_data_format, sizeof(U8), 1, file);
          }
          if (set_point_data_record_length != -1) {
            U16 point_data_record_length = U16_CLAMP(set_point_data_record_length);
            fseek(file, 105, SEEK_SET);
            fwrite(&point_data_record_length, sizeof(U16), 1, file);
          }
          if (set_number_of_point_records != -1) {
            fseek(file, 107, SEEK_SET);
            fwrite(&set_number_of_point_records, sizeof(I32), 1, file);
          }
          if (set_number_of_points_by_return[0] != -1) {
            fseek(file, 111, SEEK_SET);
            fwrite(&(set_number_of_points_by_return[0]), sizeof(I32), 1, file);
          }
          if (set_number_of_points_by_return[1] != -1) {
            fseek(file, 115, SEEK_SET);
            fwrite(&(set_number_of_points_by_return[1]), sizeof(I32), 1, file);
          }
          if (set_number_of_points_by_return[2] != -1) {
            fseek(file, 119, SEEK_SET);
            fwrite(&(set_number_of_points_by_return[2]), sizeof(I32), 1, file);
          }
          if (set_number_of_points_by_return[3] != -1) {
            fseek(file, 123, SEEK_SET);
            fwrite(&(set_number_of_points_by_return[3]), sizeof(I32), 1, file);
          }
          if (set_number_of_points_by_return[4] != -1) {
            fseek(file, 127, SEEK_SET);
            fwrite(&(set_number_of_points_by_return[4]), sizeof(I32), 1, file);
          }
          if (set_scale) {
            fseek(file, 131, SEEK_SET);
            fwrite(set_scale, 3 * sizeof(F64), 1, file);
            if (do_scale_header)  // clear offset on file-based-offset
            {
              delete[] set_scale;
              set_scale = 0;
            }
          }
          if (set_offset) {
            fseek(file, 155, SEEK_SET);
            fwrite(set_offset, 3 * sizeof(F64), 1, file);
            if (do_scale_header)  // clear offset on file-based-offset
            {
              delete[] set_offset;
              set_offset = 0;
            }
          }
          if (set_bounding_box) {
            fseek(file, 179, SEEK_SET);
            fwrite(set_bounding_box, 6 * sizeof(F64), 1, file);
            if (do_scale_header)  // clear bb on file-based-bb
            {
              delete[] set_bounding_box;
              set_bounding_box = 0;
            }
          }
          if (set_start_of_waveform_data_packet_record != -1) {
            fseek(file, 227, SEEK_SET);
            fwrite(&set_start_of_waveform_data_packet_record, sizeof(I64), 1, file);
          }
          if (set_vlr_user_id_index != -1) {
            if (set_vlr_user_id_pos != -1) {
              fseek(file, (long)set_vlr_user_id_pos, SEEK_SET);
              I32 len = (I32)strlen(set_vlr_user_id);
              for (i = 0; i < 16; i++) {
                if (i < len) {
                  fputc(set_vlr_user_id[i], file);
                } else {
                  fputc(0, file);
                }
              }
            }
          }
          if (set_vlr_record_id_index != -1) {
            if (set_vlr_record_id_pos != -1) {
              fseek(file, (long)set_vlr_record_id_pos, SEEK_SET);
              U16 record_id = (U16)set_vlr_record_id;
              fwrite(&record_id, sizeof(U16), 1, file);
            }
          }
          if (set_vlr_description_index != -1) {
            if (set_vlr_description_pos != -1) {
              fseek(file, (long)set_vlr_description_pos, SEEK_SET);
              I32 len = (I32)strlen(set_vlr_description);
              for (i = 0; i < 32; i++) {
                if (i < len) {
                  fputc(set_vlr_description[i], file);
                } else {
                  fputc(0, file);
                }
              }
            }
          }
          if (set_geotiff_epsg != -1) {
            if (set_geotiff_vlr_geo_keys_pos != -1) {
              GeoProjectionConverter geo;
              if (geo.set_epsg_code(set_geotiff_epsg)) {
                int number_of_keys;
                GeoProjectionGeoKeys* geo_keys = 0;
                int num_geo_double_params;
                double* geo_double_params = 0;
                if (geo.get_geo_keys_from_projection(number_of_keys, &geo_keys, num_geo_double_params, &geo_double_params)) {
                  U32 set_geotiff_vlr_geo_keys_new_length = sizeof(GeoProjectionGeoKeys) * (number_of_keys + 1);

                  if (set_geotiff_vlr_geo_keys_new_length <= set_geotiff_vlr_geo_keys_length) {
                    fseek(file, (long)set_geotiff_vlr_geo_keys_pos, SEEK_SET);
                    LASvlr_geo_keys vlr_geo_key_directory;
                    vlr_geo_key_directory.key_directory_version = 1;
                    vlr_geo_key_directory.key_revision = 1;
                    vlr_geo_key_directory.minor_revision = 0;
                    vlr_geo_key_directory.number_of_keys = number_of_keys;
                    fwrite(&vlr_geo_key_directory, sizeof(LASvlr_geo_keys), 1, file);
                    fwrite(geo_keys, sizeof(GeoProjectionGeoKeys), number_of_keys, file);
                    for (i = set_geotiff_vlr_geo_keys_new_length; i < (int)set_geotiff_vlr_geo_keys_length; i++) {
                      fputc(0, file);
                    }

                    if (set_geotiff_vlr_geo_double_pos != -1) {
                      fseek(file, (long)set_geotiff_vlr_geo_double_pos, SEEK_SET);
                      for (i = 0; i < (int)set_geotiff_vlr_geo_double_length; i++) {
                        fputc(0, file);
                      }
                    }

                    if (set_geotiff_vlr_geo_ascii_pos != -1) {
                      fseek(file, (long)set_geotiff_vlr_geo_ascii_pos, SEEK_SET);
                      for (i = 0; i < (int)set_geotiff_vlr_geo_ascii_length; i++) {
                        fputc(0, file);
                      }
                    }
                  } else {
                    LASMessage(
                        LAS_WARNING, "cannot set EPSG to %u because file '%s' has not enough header space for GeoTIFF tags", set_geotiff_epsg,
                        file_name);
                  }
                } else {
                  LASMessage(LAS_WARNING, "cannot set EPSG in GeoTIFF tags of because no GeoTIFF tags available for code %u", set_geotiff_epsg);
                  set_geotiff_epsg = -1;
                }
              } else {
                LASMessage(LAS_WARNING, "cannot set EPSG in GeoTIFF tags of because code %u is unknown", set_geotiff_epsg);
                set_geotiff_epsg = -1;
              }
            } else {
              LASMessage(LAS_WARNING, "cannot set EPSG to %u because file '%s' has no GeoTIFF tags", set_geotiff_epsg, file_name);
            }
          }
          LASMessage(LAS_VERBOSE, "edited '%s' ...", file_name);
          fclose(file);
        }
      }

      // open lasreader
      lasreader = lasreadopener.open();
      if (lasreader == 0) {
        laserror("cannot open lasreader");
      }

      if (delete_empty && lasreadopener.get_file_name()) {
#ifdef _WIN32
        LASMessage(LAS_VERBOSE, "delete check for '%s' with %lld points", lasreadopener.get_file_name(), lasreader->npoints);
#else
        laserror("deleting not implemented ...");
#endif
        if (lasreader->npoints == 0) {
          lasreader->close();

          char command[4096];
          snprintf(command, sizeof(command), "del \"%s\"", lasreadopener.get_file_name());
          LASMessage(LAS_VERBOSE, "executing '%s'", command);

          if (system(command) != 0) {
            laserror("failed to execute '%s'", command);
          }
        } else {
          lasreader->close();
        }

        delete lasreader;
        continue;
      }

      lasheader = &lasreader->header;

      if (base_name && lasreadopener.get_file_name()) {
        lasreader->close();

#ifdef _WIN32
        LASMessage(LAS_VERBOSE, "renaming '%s' with %lld points", lasreadopener.get_file_name(), lasreader->npoints);
#else
        laserror("renaming not implemented ...");
#endif

        char command[4096];
        if (strlen(base_name)) {
          snprintf(
              command, sizeof(command), "rename \"%s\" \"%s_%d_%d.xxx\"", lasreadopener.get_file_name(), base_name, I32_QUANTIZE(lasheader->min_x),
              I32_QUANTIZE(lasheader->min_y));
        } else {
          snprintf(
              command, sizeof(command), "rename \"%s\" \"%d_%d.xxx\"", lasreadopener.get_file_name(), I32_QUANTIZE(lasheader->min_x),
              I32_QUANTIZE(lasheader->min_y));
        }
        int len1 = (int)strlen(lasreadopener.get_file_name());
        int len2 = (int)strlen(command);
        command[len2 - 4] = lasreadopener.get_file_name()[len1 - 3];
        command[len2 - 3] = lasreadopener.get_file_name()[len1 - 2];
        command[len2 - 2] = lasreadopener.get_file_name()[len1 - 1];
        delete lasreader;

        LASMessage(LAS_VERBOSE, "executing '%s'", command);

        if (system(command) != 0) {
          laserror("failed to execute '%s'", command);
        }
        continue;
      }

      LASMessage(
          LAS_VERBOSE, "%s '%s' with %lld points", (repair_bb || repair_counters ? "repairing" : "reading"),
          (lasreadopener.get_file_name() ? lasreadopener.get_file_name() : "stdin"), lasreader->npoints);
      if (auto_date_creation && lasreadopener.get_file_name()) {
#ifdef _WIN32
        WIN32_FILE_ATTRIBUTE_DATA attr;
        SYSTEMTIME creation;
        GetFileAttributesEx(lasreadopener.get_file_name(), GetFileExInfoStandard, &attr);
        FileTimeToSystemTime(&attr.ftCreationTime, &creation);
        int startday[13] = {-1, 0, 31, 59, 90, 120, 151, 181, 212, 243, 273, 304, 334};
        set_creation_day = startday[creation.wMonth] + creation.wDay;
        set_creation_year = creation.wYear;
        // leap year handling
        if ((((creation.wYear) % 4) == 0) && (creation.wMonth > 2)) set_creation_day++;
        edit_header = true;
#endif
      }

      if (laswriteopener.get_file_name() == 0) {
        if (lasreadopener.get_file_name() &&
            ((laswriteopener.get_format() == LAS_TOOLS_FORMAT_TXT || laswriteopener.get_format() == LAS_TOOLS_FORMAT_JSON))) {
          laswriteopener.make_file_name(lasreadopener.get_file_name(), -2);
        }
      }

      if (laswriteopener.get_file_name()) {
        // make sure we do not corrupt the input file
        if (lasreadopener.get_file_name() && (strcmp(lasreadopener.get_file_name(), laswriteopener.get_file_name()) == 0)) {
          laserror("input and output file name for '%s' are identical", lasreadopener.get_file_name());
        }
        // open the text output file
        file_out = LASfopen(laswriteopener.get_file_name(), "w");
        if (file_out == 0) {
          LASMessage(LAS_WARNING, "could not open output text file '%s'", laswriteopener.get_file_name());
          file_out = stderr;
        }
      }
      // If a json output file was specified in the call, the las info is also compiled in json format.
      if (laswriteopener.get_file_name() && laswriteopener.get_format() == LAS_TOOLS_FORMAT_JSON) {
        json_out = true;
      }

      // print name of input
      JsonObject json_sub_main;

      if (file_out) {
        if (json_out) json_sub_main["las_json_version"] = "1.0";

        if (lasreadopener.is_merged()) {
          if (json_out) {
            json_sub_main["las_tool_version"] = LAS_TOOLS_VERSION;
            json_sub_main["merged_files"] = lasreadopener.get_file_name_number();
          } else {
            fprintf(file_out, "lasinfo (%u) report for %u merged files\012", LAS_TOOLS_VERSION, lasreadopener.get_file_name_number());
          }
        } else if (lasreadopener.is_piped()) {
          if (json_out) {
            json_sub_main["las_tool_version"] = LAS_TOOLS_VERSION;
            json_sub_main["report"] = "piped input";
          } else {
            fprintf(file_out, "lasinfo (%u) report for piped input\012", LAS_TOOLS_VERSION);
          }
        } else if (lasreadopener.get_file_name()) {
          if (json_out) {
            json_sub_main["las_tool_version"] = LAS_TOOLS_VERSION;
            json_sub_main["input_file_name"] = lasreadopener.get_file_name();
          } else {
            fprintf(file_out, "lasinfo (%u) report for '%s'\012", LAS_TOOLS_VERSION, lasreadopener.get_file_name());
          }
        }
      }

      U32 number_of_point_records = lasheader->number_of_point_records;
      U32 number_of_points_by_return0 = lasheader->number_of_points_by_return[0];

      // print header info
      CHAR printstring[4096];

      if (file_out && !no_header) {
        JsonObject json_sub_main_header_entries;

        if (lasreadopener.is_merged() && (lasreader->header.version_minor < 4)) {
          if (lasreader->npoints > number_of_point_records) {
            if (json_out) {
              char buffer[256];
              snprintf(
                  buffer, sizeof(buffer), "merged file has %lld points, more than the 32 bits counters of LAS 1.%d can handle.\012",
                  lasreader->npoints, lasreader->header.version_minor);
              json_sub_main_header_entries["warnings"].push_back(buffer);
            } else {
              fprintf(
                  file_out, "WARNING: merged file has %lld points, more than the 32 bits counters of LAS 1.%d can handle.\012", lasreader->npoints,
                  lasreader->header.version_minor);
            }
          }
        }
        if (json_out) {
          json_sub_main_header_entries["file_signature"] = std::string(lasheader->file_signature).substr(0, 4);
          json_sub_main_header_entries["file_source_id"] = lasheader->file_source_ID;
          json_sub_main_header_entries["global_encoding"] = lasheader->global_encoding;
          json_sub_main_header_entries["project_id_guid_data"] = lasheader->get_GUID();
          json_sub_main_header_entries["version_major_minor"] = lasheader->get_version();
          json_sub_main_header_entries["system_identifier"] = std::string(lasheader->system_identifier).substr(0, 32);
          json_sub_main_header_entries["generating_software"] = std::string(lasheader->generating_software).substr(0, 32);
          json_sub_main_header_entries["file_creation_day"] = lasheader->file_creation_day;
          json_sub_main_header_entries["file_creation_year"] = lasheader->file_creation_year;
          json_sub_main_header_entries["header_size"] = lasheader->header_size;
          json_sub_main_header_entries["offset_to_point_data"] = lasheader->offset_to_point_data;
          json_sub_main_header_entries["number_of_variable_length_records"] = lasheader->number_of_variable_length_records;
          json_sub_main_header_entries["point_data_format"] = lasheader->point_data_format;
          json_sub_main_header_entries["point_data_record_length"] = lasheader->point_data_record_length;
          json_sub_main_header_entries["number_of_point_records"] = lasheader->number_of_point_records;
          json_sub_main_header_entries["number_of_points_by_return"] = {
              lasheader->number_of_points_by_return[0], lasheader->number_of_points_by_return[1], lasheader->number_of_points_by_return[2],
              lasheader->number_of_points_by_return[3], lasheader->number_of_points_by_return[4]};
          JsonObject json_scale_factor;
          lidardouble2string(printstring, lasheader->x_scale_factor);
          json_scale_factor["x"] = parseFormattedDouble(printstring);
          lidardouble2string(printstring, lasheader->y_scale_factor);
          json_scale_factor["y"] = parseFormattedDouble(printstring);
          lidardouble2string(printstring, lasheader->z_scale_factor);
          json_scale_factor["z"] = parseFormattedDouble(printstring);
          json_sub_main_header_entries["scale_factor"] = json_scale_factor;
          JsonObject json_offset_factor;
          lidardouble2string(printstring, lasheader->x_offset);
          json_offset_factor["x"] = parseFormattedDouble(printstring);
          lidardouble2string(printstring, lasheader->y_offset);
          json_offset_factor["y"] = parseFormattedDouble(printstring);
          lidardouble2string(printstring, lasheader->z_offset);
          json_offset_factor["z"] = parseFormattedDouble(printstring);
          json_sub_main_header_entries["offset"] = json_offset_factor;
          JsonObject json_min_value;
          lidardouble2string(printstring, lasheader->min_x, lasheader->x_scale_factor);
          json_min_value["x"] = parseFormattedDouble(printstring);
          lidardouble2string(printstring, lasheader->min_y, lasheader->y_scale_factor);
          json_min_value["y"] = parseFormattedDouble(printstring);
          lidardouble2string(printstring, lasheader->min_z, lasheader->z_scale_factor);
          json_min_value["z"] = parseFormattedDouble(printstring);
          json_sub_main_header_entries["min"] = json_min_value;
          JsonObject json_max_value;
          lidardouble2string(printstring, lasheader->max_x, lasheader->x_scale_factor);
          json_max_value["x"] = parseFormattedDouble(printstring);
          lidardouble2string(printstring, lasheader->max_y, lasheader->y_scale_factor);
          json_max_value["y"] = parseFormattedDouble(printstring);
          lidardouble2string(printstring, lasheader->max_z, lasheader->z_scale_factor);
          json_max_value["z"] = parseFormattedDouble(printstring);
          json_sub_main_header_entries["max"] = json_max_value;
        } else {
          fprintf(file_out, "reporting all LAS header entries:\012");
          fprintf(file_out, "  file signature:             '%.4s'\012", lasheader->file_signature);
          fprintf(file_out, "  file source ID:             %d\012", lasheader->file_source_ID);
          fprintf(file_out, "  global_encoding:            %d\012", lasheader->global_encoding);
          fprintf(file_out, "  project ID GUID data 1-4:   %s\012", lasheader->get_GUID().c_str());
          fprintf(file_out, "  version major.minor:        %s\012", lasheader->get_version().c_str());
          fprintf(file_out, "  system identifier:          '%.32s'\012", lasheader->system_identifier);
          fprintf(file_out, "  generating software:        '%.32s'\012", lasheader->generating_software);
          fprintf(file_out, "  file creation day/year:     %d/%d\012", lasheader->file_creation_day, lasheader->file_creation_year);
          fprintf(file_out, "  header size:                %d\012", lasheader->header_size);
          fprintf(file_out, "  offset to point data:       %u\012", lasheader->offset_to_point_data);
          fprintf(file_out, "  number var. length records: %u\012", lasheader->number_of_variable_length_records);
          fprintf(file_out, "  point data format:          %d\012", lasheader->point_data_format);
          fprintf(file_out, "  point data record length:   %d\012", lasheader->point_data_record_length);
          fprintf(file_out, "  number of point records:    %u\012", lasheader->number_of_point_records);
          fprintf(
              file_out, "  number of points by return: %u %u %u %u %u\012", lasheader->number_of_points_by_return[0],
              lasheader->number_of_points_by_return[1], lasheader->number_of_points_by_return[2], lasheader->number_of_points_by_return[3],
              lasheader->number_of_points_by_return[4]);
          fprintf(file_out, "  scale factor x y z:         ");
          lidardouble2string(printstring, lasheader->x_scale_factor);
          fprintf(file_out, "%s ", printstring);
          lidardouble2string(printstring, lasheader->y_scale_factor);
          fprintf(file_out, "%s ", printstring);
          lidardouble2string(printstring, lasheader->z_scale_factor);
          fprintf(file_out, "%s\012", printstring);
          fprintf(file_out, "  offset x y z:               ");
          lidardouble2string(printstring, lasheader->x_offset);
          fprintf(file_out, "%s ", printstring);
          lidardouble2string(printstring, lasheader->y_offset);
          fprintf(file_out, "%s ", printstring);
          lidardouble2string(printstring, lasheader->z_offset);
          fprintf(file_out, "%s\012", printstring);
          fprintf(file_out, "  min x y z:                  ");
          lidardouble2string(printstring, lasheader->min_x, lasheader->x_scale_factor);
          fprintf(file_out, "%s ", printstring);
          lidardouble2string(printstring, lasheader->min_y, lasheader->y_scale_factor);
          fprintf(file_out, "%s ", printstring);
          lidardouble2string(printstring, lasheader->min_z, lasheader->z_scale_factor);
          fprintf(file_out, "%s\012", printstring);
          fprintf(file_out, "  max x y z:                  ");
          lidardouble2string(printstring, lasheader->max_x, lasheader->x_scale_factor);
          fprintf(file_out, "%s ", printstring);
          lidardouble2string(printstring, lasheader->max_y, lasheader->y_scale_factor);
          fprintf(file_out, "%s ", printstring);
          lidardouble2string(printstring, lasheader->max_z, lasheader->z_scale_factor);
          fprintf(file_out, "%s\012", printstring);
        }
        if (!no_warnings && !valid_resolution(lasheader->min_x, lasheader->x_offset, lasheader->x_scale_factor)) {
          if (json_out) {
            char buffer[256];
            lidardouble2string(printstring, lasheader->min_x);
            snprintf(buffer, sizeof(buffer), "Stored resolution of min_x not compatible with x_offset and x_scale_factor: %s", printstring);
            json_sub_main_header_entries["warnings"].push_back(buffer);
          } else {
            fprintf(file_out, "WARNING: stored resolution of min_x not compatible with x_offset and x_scale_factor: ");
            lidardouble2string(printstring, lasheader->min_x);
            fprintf(file_out, "%s\n", printstring);
          }
        }
        if (!no_warnings && !valid_resolution(lasheader->min_y, lasheader->y_offset, lasheader->y_scale_factor)) {
          if (json_out) {
            char buffer[256];
            lidardouble2string(printstring, lasheader->min_y);
            snprintf(buffer, sizeof(buffer), "Stored resolution of min_y not compatible with y_offset and y_scale_factor: %s", printstring);
            json_sub_main_header_entries["warnings"].push_back(buffer);
          } else {
            fprintf(file_out, "WARNING: stored resolution of min_y not compatible with y_offset and y_scale_factor: ");
            lidardouble2string(printstring, lasheader->min_y);
            fprintf(file_out, "%s\n", printstring);
          }
        }
        if (!no_warnings && !valid_resolution(lasheader->min_z, lasheader->z_offset, lasheader->z_scale_factor)) {
          if (json_out) {
            char buffer[256];
            lidardouble2string(printstring, lasheader->min_z);
            snprintf(buffer, sizeof(buffer), "Stored resolution of min_z not compatible with z_offset and z_scale_factor: %s", printstring);
            json_sub_main_header_entries["warnings"].push_back(buffer);
          } else {
            fprintf(file_out, "WARNING: stored resolution of min_z not compatible with z_offset and z_scale_factor: ");
            lidardouble2string(printstring, lasheader->min_z);
            fprintf(file_out, "%s\n", printstring);
          }
        }
        if (!no_warnings && !valid_resolution(lasheader->max_x, lasheader->x_offset, lasheader->x_scale_factor)) {
          if (json_out) {
            char buffer[256];
            lidardouble2string(printstring, lasheader->max_x);
            snprintf(buffer, sizeof(buffer), "Stored resolution of max_x not compatible with x_offset and x_scale_factor: %s", printstring);
            json_sub_main_header_entries["warnings"].push_back(buffer);
          } else {
            fprintf(file_out, "WARNING: stored resolution of max_x not compatible with x_offset and x_scale_factor: ");
            lidardouble2string(printstring, lasheader->max_x);
            fprintf(file_out, "%s\n", printstring);
          }
        }
        if (!no_warnings && !valid_resolution(lasheader->max_y, lasheader->y_offset, lasheader->y_scale_factor)) {
          if (json_out) {
            char buffer[256];
            lidardouble2string(printstring, lasheader->max_y);
            snprintf(buffer, sizeof(buffer), "Stored resolution of max_y not compatible with y_offset and y_scale_factor: %s", printstring);
            json_sub_main_header_entries["warnings"].push_back(buffer);
          } else {
            fprintf(file_out, "WARNING: stored resolution of max_y not compatible with y_offset and y_scale_factor: ");
            lidardouble2string(printstring, lasheader->max_y);
            fprintf(file_out, "%s\n", printstring);
          }
        }
        if (!no_warnings && !valid_resolution(lasheader->max_z, lasheader->z_offset, lasheader->z_scale_factor)) {
          if (json_out) {
            char buffer[256];
            lidardouble2string(printstring, lasheader->max_z);
            snprintf(buffer, sizeof(buffer), "Stored resolution of max_z not compatible with z_offset and z_scale_factor: %s", printstring);
            json_sub_main_header_entries["warnings"].push_back(buffer);
          } else {
            fprintf(file_out, "WARNING: stored resolution of max_z not compatible with z_offset and z_scale_factor: ");
            lidardouble2string(printstring, lasheader->max_z);
            fprintf(file_out, "%s\n", printstring);
          }
        }
        if ((lasheader->version_major == 1) && (lasheader->version_minor >= 3)) {
          if (json_out) {
            json_sub_main_header_entries["start_record_waveform_data_packet"] = lasheader->start_of_waveform_data_packet_record;
          } else {
            fprintf(file_out, "  start of waveform data packet record: %lld\012", lasheader->start_of_waveform_data_packet_record);
          }
        }
        if ((lasheader->version_major == 1) && (lasheader->version_minor >= 4)) {
          if (json_out) {
            json_sub_main_header_entries["start_of_first_extended_vlr"] = lasheader->start_of_first_extended_variable_length_record;
            json_sub_main_header_entries["number_of_extended_vlrs"] = lasheader->number_of_extended_variable_length_records;
            json_sub_main_header_entries["extended_number_of_point_records"] = lasheader->extended_number_of_point_records;
            JsonObject json_extended_points;
            for (int i = 0; i < 15; ++i) {
              json_extended_points.push_back(lasheader->extended_number_of_points_by_return[i]);
            }
            json_sub_main_header_entries["extended_number_of_points_by_return"] = json_extended_points;
          } else {
            fprintf(
                file_out, "  start of first extended variable length record: %lld\012", lasheader->start_of_first_extended_variable_length_record);
            fprintf(file_out, "  number of extended_variable length records: %d\012", lasheader->number_of_extended_variable_length_records);
            fprintf(file_out, "  extended number of point records: %lld\012", lasheader->extended_number_of_point_records);
            fprintf(
                file_out, "  extended number of points by return: %lld %lld %lld %lld %lld %lld %lld %lld %lld %lld %lld %lld %lld %lld %lld\012",
                lasheader->extended_number_of_points_by_return[0], lasheader->extended_number_of_points_by_return[1],
                lasheader->extended_number_of_points_by_return[2], lasheader->extended_number_of_points_by_return[3],
                lasheader->extended_number_of_points_by_return[4], lasheader->extended_number_of_points_by_return[5],
                lasheader->extended_number_of_points_by_return[6], lasheader->extended_number_of_points_by_return[7],
                lasheader->extended_number_of_points_by_return[8], lasheader->extended_number_of_points_by_return[9],
                lasheader->extended_number_of_points_by_return[10], lasheader->extended_number_of_points_by_return[11],
                lasheader->extended_number_of_points_by_return[12], lasheader->extended_number_of_points_by_return[13],
                lasheader->extended_number_of_points_by_return[14]);
          }
        }
        if (lasheader->user_data_in_header_size) {
          if (json_out) {
            json_sub_main_header_entries["user_defined_bytes"] = lasheader->user_data_in_header_size;
          } else {
            fprintf(file_out, "the header contains %u user-defined bytes\012", lasheader->user_data_in_header_size);
          }
        }
        if (json_out && !json_sub_main_header_entries.is_null()) json_sub_main["las_header_entries"] = json_sub_main_header_entries;
      }
      // maybe print variable header
      if (file_out && !no_variable_header) {
        for (int i = 0; i < (int)lasheader->number_of_variable_length_records; i++) {
          JsonObject json_vlr_record;

          if (json_out) {
            json_vlr_record["record_number"] = i + 1;
            json_vlr_record["total_records"] = (int)lasheader->number_of_variable_length_records;
            json_vlr_record["reserved"] = lasreader->header.vlrs[i].reserved;
            json_vlr_record["user_id"] = std::string(lasreader->header.vlrs[i].user_id).substr(0, 16);
            json_vlr_record["record_id"] = lasreader->header.vlrs[i].record_id;
            json_vlr_record["record_length_after_header"] = lasreader->header.vlrs[i].record_length_after_header;
            json_vlr_record["description"] = std::string(lasreader->header.vlrs[i].description).substr(0, 32);
          } else {
            fprintf(file_out, "variable length header record %d of %d:\012", i + 1, (int)lasheader->number_of_variable_length_records);
            fprintf(file_out, "  reserved             %d\012", lasreader->header.vlrs[i].reserved);
            fprintf(file_out, "  user ID              '%.16s'\012", lasreader->header.vlrs[i].user_id);
            fprintf(file_out, "  record ID            %d\012", lasreader->header.vlrs[i].record_id);
            fprintf(file_out, "  length after header  %d\012", lasreader->header.vlrs[i].record_length_after_header);
            fprintf(file_out, "  description          '%.32s'\012", lasreader->header.vlrs[i].description);
          }

          // special handling for known variable header tags

          if ((strcmp(lasheader->vlrs[i].user_id, "LASF_Projection") == 0) && (lasheader->vlrs[i].data != 0)) {
            if (lasheader->vlrs[i].record_id == 34735) {  // GeoKeyDirectoryTag
              if (json_out) {
                json_vlr_record["geo_key_directory_tag"];
                char buffer[256];
                snprintf(
                    buffer, sizeof(buffer), "%d.%d.%d", lasheader->vlr_geo_keys->key_directory_version, lasheader->vlr_geo_keys->key_revision,
                    lasheader->vlr_geo_keys->minor_revision);
                json_vlr_record["geo_key_directory_tag"]["geo_key_version"] = buffer;
                json_vlr_record["geo_key_directory_tag"]["number_of_keys"] = lasheader->vlr_geo_keys->number_of_keys;
              } else {
                fprintf(
                    file_out, "    GeoKeyDirectoryTag version %d.%d.%d number of keys %d\012", lasheader->vlr_geo_keys->key_directory_version,
                    lasheader->vlr_geo_keys->key_revision, lasheader->vlr_geo_keys->minor_revision, lasheader->vlr_geo_keys->number_of_keys);
              }

              for (int j = 0; j < lasheader->vlr_geo_keys->number_of_keys; j++) {
                JsonObject json_geo_key_entry;
                if (json_out) json_vlr_record["geo_key_directory_tag"]["geo_keys"];

                if (file_out) {
                  if (json_out) {
                    json_geo_key_entry["key"] = lasheader->vlr_geo_key_entries[j].key_id;
                    json_geo_key_entry["tiff_tag_location"] = lasheader->vlr_geo_key_entries[j].tiff_tag_location;
                    json_geo_key_entry["count"] = lasheader->vlr_geo_key_entries[j].count;
                    json_geo_key_entry["value_offset"] = lasheader->vlr_geo_key_entries[j].value_offset;
                  } else {
                    fprintf(
                        file_out, "      key %d tiff_tag_location %d count %d value_offset %d - ", lasheader->vlr_geo_key_entries[j].key_id,
                        lasheader->vlr_geo_key_entries[j].tiff_tag_location, lasheader->vlr_geo_key_entries[j].count,
                        lasheader->vlr_geo_key_entries[j].value_offset);
                  }
                  switch (lasreader->header.vlr_geo_key_entries[j].key_id) {
                    case 1024:  // GTModelTypeGeoKey
                      switch (lasreader->header.vlr_geo_key_entries[j].value_offset) {
                        case 1:  // ModelTypeProjected
                          if (json_out) {
                            json_geo_key_entry["gt_model_type_geo_key"] = "ModelTypeProjected";
                          } else {
                            fprintf(file_out, "GTModelTypeGeoKey: ModelTypeProjected\012");
                          }
                          break;
                        case 2:
                          if (json_out) {
                            json_geo_key_entry["gt_model_type_geo_key"] = "ModelTypeGeographic";
                          } else {
                            fprintf(file_out, "GTModelTypeGeoKey: ModelTypeGeographic\012");
                          }
                          break;
                        case 3:
                          if (json_out) {
                            json_geo_key_entry["gt_model_type_geo_key"] = "ModelTypeGeocentric";
                          } else {
                            fprintf(file_out, "GTModelTypeGeoKey: ModelTypeGeocentric\012");
                          }
                          break;
                        case 0:  // ModelTypeUndefined
                          if (json_out) {
                            json_geo_key_entry["gt_model_type_geo_key"] = "ModelTypeUndefined";
                          } else {
                            fprintf(file_out, "GTModelTypeGeoKey: ModelTypeUndefined\012");
                          }
                          break;
                        default:
                          if (json_out) {
                            char buffer[256];
                            snprintf(buffer, sizeof(buffer), "look-up for %d not implemented", lasreader->header.vlr_geo_key_entries[j].value_offset);
                            json_geo_key_entry["gt_model_type_geo_key"] = buffer;
                          } else {
                            fprintf(
                                file_out, "GTModelTypeGeoKey: look-up for %d not implemented\012",
                                lasreader->header.vlr_geo_key_entries[j].value_offset);
                          }
                      }
                      break;
                    case 1025:  // GTRasterTypeGeoKey
                      switch (lasreader->header.vlr_geo_key_entries[j].value_offset) {
                        case 1:  // RasterPixelIsArea
                          if (json_out) {
                            json_geo_key_entry["gt_raster_type_geo_key"] = "RasterPixelIsArea";
                          } else {
                            fprintf(file_out, "GTRasterTypeGeoKey: RasterPixelIsArea\012");
                          }
                          break;
                        case 2:  // RasterPixelIsPoint
                          if (json_out) {
                            json_geo_key_entry["gt_raster_type_geo_key"] = "RasterPixelIsPoint";
                          } else {
                            fprintf(file_out, "GTRasterTypeGeoKey: RasterPixelIsPoint\012");
                          }
                          break;
                        default:
                          if (json_out) {
                            char buffer[256];
                            snprintf(buffer, sizeof(buffer), "look-up for %d not implemented", lasreader->header.vlr_geo_key_entries[j].value_offset);
                            json_geo_key_entry["gt_raster_type_geo_key"] = buffer;
                          } else {
                            fprintf(
                                file_out, "GTRasterTypeGeoKey: look-up for %d not implemented\012",
                                lasreader->header.vlr_geo_key_entries[j].value_offset);
                          }
                      }
                      break;
                    case 1026:  // GTCitationGeoKey
                      if (lasreader->header.vlr_geo_ascii_params) {
                        char dummy[256];
                        strncpy_las(
                            dummy, sizeof(dummy), &(lasreader->header.vlr_geo_ascii_params[lasreader->header.vlr_geo_key_entries[j].value_offset]),
                            lasreader->header.vlr_geo_key_entries[j].count);
                        dummy[lasreader->header.vlr_geo_key_entries[j].count - 1] = '\0';
                        if (json_out) {
                          json_geo_key_entry["gt_citation_geo_key"] = dummy;
                        } else {
                          fprintf(file_out, "GTCitationGeoKey: %s\012", dummy);
                        }
                      }
                      break;
                    case 2048:  // GeographicTypeGeoKey
                      switch (lasreader->header.vlr_geo_key_entries[j].value_offset) {
                        case 32767:  // user-defined
                          if (json_out) {
                            json_geo_key_entry["geographic_type_geo_key"] = "user-defined";
                          } else {
                            fprintf(file_out, "GeographicTypeGeoKey: user-defined\012");
                          }
                          break;
                        case 4001:  // GCSE_Airy1830
                          if (json_out) {
                            json_geo_key_entry["geographic_type_geo_key"] = "GCSE_Airy1830";
                          } else {
                            fprintf(file_out, "GeographicTypeGeoKey: GCSE_Airy1830\012");
                          }
                          break;
                        case 4002:  // GCSE_AiryModified1849
                          if (json_out) {
                            json_geo_key_entry["geographic_type_geo_key"] = "GCSE_AiryModified1849";
                          } else {
                            fprintf(file_out, "GeographicTypeGeoKey: GCSE_AiryModified1849\012");
                          }
                          break;
                        case 4003:  // GCSE_AustralianNationalSpheroid
                          if (json_out) {
                            json_geo_key_entry["geographic_type_geo_key"] = "GCSE_AustralianNationalSpheroid";
                          } else {
                            fprintf(file_out, "GeographicTypeGeoKey: GCSE_AustralianNationalSpheroid\012");
                          }
                          break;
                        case 4004:  // GCSE_Bessel1841
                          if (json_out) {
                            json_geo_key_entry["geographic_type_geo_key"] = "GCSE_Bessel1841";
                          } else {
                            fprintf(file_out, "GeographicTypeGeoKey: GCSE_Bessel1841\012");
                          }
                          break;
                        case 4005:  // GCSE_Bessel1841Modified
                          if (json_out) {
                            json_geo_key_entry["geographic_type_geo_key"] = "GCSE_Bessel1841Modified";
                          } else {
                            fprintf(file_out, "GeographicTypeGeoKey: GCSE_Bessel1841Modified\012");
                          }
                          break;
                        case 4006:  // GCSE_BesselNamibia
                          if (json_out) {
                            json_geo_key_entry["geographic_type_geo_key"] = "GCSE_BesselNamibia";
                          } else {
                            fprintf(file_out, "GeographicTypeGeoKey: GCSE_BesselNamibia\012");
                          }
                          break;
                        case 4008:  // GCSE_Clarke1866
                          if (json_out) {
                            json_geo_key_entry["geographic_type_geo_key"] = "GCSE_Clarke1866";
                          } else {
                            fprintf(file_out, "GeographicTypeGeoKey: GCSE_Clarke1866\012");
                          }
                          break;
                        case 4009:  // GCSE_Clarke1866Michigan
                          if (json_out) {
                            json_geo_key_entry["geographic_type_geo_key"] = "GCSE_Clarke1866Michigan";
                          } else {
                            fprintf(file_out, "GeographicTypeGeoKey: GCSE_Clarke1866Michigan\012");
                          }
                          break;
                        case 4010:  // GCSE_Clarke1880_Benoit
                          if (json_out) {
                            json_geo_key_entry["geographic_type_geo_key"] = "GCSE_Clarke1880_Benoit";
                          } else {
                            fprintf(file_out, "GeographicTypeGeoKey: GCSE_Clarke1880_Benoit\012");
                          }
                          break;
                        case 4011:  // GCSE_Clarke1880_IGN
                          if (json_out) {
                            json_geo_key_entry["geographic_type_geo_key"] = "GCSE_Clarke1880_IGN";
                          } else {
                            fprintf(file_out, "GeographicTypeGeoKey: GCSE_Clarke1880_IGN\012");
                          }
                          break;
                        case 4012:  // GCSE_Clarke1880_RGS
                          if (json_out) {
                            json_geo_key_entry["geographic_type_geo_key"] = "GCSE_Clarke1880_RGS";
                          } else {
                            fprintf(file_out, "GeographicTypeGeoKey: GCSE_Clarke1880_RGS\012");
                          }
                          break;
                        case 4013:  // GCSE_Clarke1880_Arc
                          if (json_out) {
                            json_geo_key_entry["geographic_type_geo_key"] = "GCSE_Clarke1880_Arc";
                          } else {
                            fprintf(file_out, "GeographicTypeGeoKey: GCSE_Clarke1880_Arc\012");
                          }
                          break;
                        case 4014:  // GCSE_Clarke1880_SGA1922
                          if (json_out) {
                            json_geo_key_entry["geographic_type_geo_key"] = "GCSE_Clarke1880_SGA1922";
                          } else {
                            fprintf(file_out, "GeographicTypeGeoKey: GCSE_Clarke1880_SGA1922\012");
                          }
                          break;
                        case 4015:  // GCSE_Everest1830_1937Adjustment
                          if (json_out) {
                            json_geo_key_entry["geographic_type_geo_key"] = "GCSE_Everest1830_1937Adjustment";
                          } else {
                            fprintf(file_out, "GeographicTypeGeoKey: GCSE_Everest1830_1937Adjustment\012");
                          }
                          break;
                        case 4016:  // GCSE_Everest1830_1967Definition
                          if (json_out) {
                            json_geo_key_entry["geographic_type_geo_key"] = "GCSE_Everest1830_1967Definition";
                          } else {
                            fprintf(file_out, "GeographicTypeGeoKey: GCSE_Everest1830_1967Definition\012");
                          }
                          break;
                        case 4017:  // GCSE_Everest1830_1975Definition
                          if (json_out) {
                            json_geo_key_entry["geographic_type_geo_key"] = "GCSE_Everest1830_1975Definition";
                          } else {
                            fprintf(file_out, "GeographicTypeGeoKey: GCSE_Everest1830_1975Definition\012");
                          }
                          break;
                        case 4018:  // GCSE_Everest1830Modified
                          if (json_out) {
                            json_geo_key_entry["geographic_type_geo_key"] = "GCSE_Everest1830Modified";
                          } else {
                            fprintf(file_out, "GeographicTypeGeoKey: GCSE_Everest1830Modified\012");
                          }
                          break;
                        case 4019:  // GCSE_GRS1980
                          if (json_out) {
                            json_geo_key_entry["geographic_type_geo_key"] = "GCSE_GRS1980";
                          } else {
                            fprintf(file_out, "GeographicTypeGeoKey: GCSE_GRS1980\012");
                          }
                          break;
                        case 4020:  // GCSE_Helmert1906
                          if (json_out) {
                            json_geo_key_entry["geographic_type_geo_key"] = "GCSE_Helmert1906";
                          } else {
                            fprintf(file_out, "GeographicTypeGeoKey: GCSE_Helmert1906\012");
                          }
                          break;
                        case 4022:  // GCSE_International1924
                          if (json_out) {
                            json_geo_key_entry["geographic_type_geo_key"] = "GCSE_International1924";
                          } else {
                            fprintf(file_out, "GeographicTypeGeoKey: GCSE_International1924\012");
                          }
                          break;
                        case 4023:  // GCSE_International1967
                          if (json_out) {
                            json_geo_key_entry["geographic_type_geo_key"] = "GCSE_International1967";
                          } else {
                            fprintf(file_out, "GeographicTypeGeoKey: GCSE_International1967\012");
                          }
                          break;
                        case 4024:  // GCSE_Krassowsky1940
                          if (json_out) {
                            json_geo_key_entry["geographic_type_geo_key"] = "GCSE_Krassowsky1940";
                          } else {
                            fprintf(file_out, "GeographicTypeGeoKey: GCSE_Krassowsky1940\012");
                          }
                          break;
                        case 4030:  // GCSE_WGS84
                          if (json_out) {
                            json_geo_key_entry["geographic_type_geo_key"] = "GCSE_WGS84";
                          } else {
                            fprintf(file_out, "GeographicTypeGeoKey: GCSE_WGS84\012");
                          }
                          break;
                        case 4034:  // GCSE_Clarke1880
                          if (json_out) {
                            json_geo_key_entry["geographic_type_geo_key"] = "GCSE_Clarke1880";
                          } else {
                            fprintf(file_out, "GeographicTypeGeoKey: GCSE_Clarke1880\012");
                          }
                          break;
                        case 4140:  // GCSE_NAD83_CSRS
                          if (json_out) {
                            json_geo_key_entry["geographic_type_geo_key"] = "GCSE_NAD83_CSRS";
                          } else {
                            fprintf(file_out, "GeographicTypeGeoKey: GCSE_NAD83_CSRS\012");
                          }
                          break;
                        case 4167:  // GCSE_New_Zealand_Geodetic_Datum_2000
                          if (json_out) {
                            json_geo_key_entry["geographic_type_geo_key"] = "GCSE_New_Zealand_Geodetic_Datum_2000";
                          } else {
                            fprintf(file_out, "GeographicTypeGeoKey: GCSE_New_Zealand_Geodetic_Datum_2000\012");
                          }
                          break;
                        case 4267:  // GCS_NAD27
                          if (json_out) {
                            json_geo_key_entry["geographic_type_geo_key"] = "GCS_NAD27";
                          } else {
                            fprintf(file_out, "GeographicTypeGeoKey: GCS_NAD27\012");
                          }
                          break;
                        case 4269:  // GCS_NAD83
                          if (json_out) {
                            json_geo_key_entry["geographic_type_geo_key"] = "GCS_NAD83";
                          } else {
                            fprintf(file_out, "GeographicTypeGeoKey: GCS_NAD83\012");
                          }
                          break;
                        case 4283:  // GCS_GDA94
                          if (json_out) {
                            json_geo_key_entry["geographic_type_geo_key"] = "GCS_GDA94";
                          } else {
                            fprintf(file_out, "GeographicTypeGeoKey: GCS_GDA94\012");
                          }
                          break;
                        case 4312:  // GCS_MGI
                          if (json_out) {
                            json_geo_key_entry["geographic_type_geo_key"] = "GCS_MGI";
                          } else {
                            fprintf(file_out, "GeographicTypeGeoKey: GCS_MGI\012");
                          }
                          break;
                        case 4322:  // GCS_WGS_72
                          if (json_out) {
                            json_geo_key_entry["geographic_type_geo_key"] = "GCS_WGS_72";
                          } else {
                            fprintf(file_out, "GeographicTypeGeoKey: GCS_WGS_72\012");
                          }
                          break;
                        case 4326:  // GCS_WGS_84
                          if (json_out) {
                            json_geo_key_entry["geographic_type_geo_key"] = "GCS_WGS_84";
                          } else {
                            fprintf(file_out, "GeographicTypeGeoKey: GCS_WGS_84\012");
                          }
                          break;
                        case 4289:  // GCS_Amersfoort
                          if (json_out) {
                            json_geo_key_entry["geographic_type_geo_key"] = "GCS_Amersfoort";
                          } else {
                            fprintf(file_out, "GeographicTypeGeoKey: GCS_Amersfoort\012");
                          }
                          break;
                        case 4617:  // GCS_NAD83_CSRS
                          if (json_out) {
                            json_geo_key_entry["geographic_type_geo_key"] = "GCS_NAD83_CSRS";
                          } else {
                            fprintf(file_out, "GeographicTypeGeoKey: GCS_NAD83_CSRS\012");
                          }
                          break;
                        case 4619:  // GCS_SWEREF99
                          if (json_out) {
                            json_geo_key_entry["geographic_type_geo_key"] = "GCS_SWEREF99";
                          } else {
                            fprintf(file_out, "GeographicTypeGeoKey: GCS_SWEREF99\012");
                          }
                          break;
                        case 6318:  // GCS_NAD83_2011
                          if (json_out) {
                            json_geo_key_entry["geographic_type_geo_key"] = "GCS_NAD83_2011";
                          } else {
                            fprintf(file_out, "GeographicTypeGeoKey: GCS_NAD83_2011\012");
                          }
                          break;
                        case 6322:  // GCS_NAD83_PA11
                          if (json_out) {
                            json_geo_key_entry["geographic_type_geo_key"] = "GCS_NAD83_PA11";
                          } else {
                            fprintf(file_out, "GeographicTypeGeoKey: GCS_NAD83_PA11\012");
                          }
                          break;
                        case 7844:  // GCS_GDA2020
                          if (json_out) {
                            json_geo_key_entry["geographic_type_geo_key"] = "GCS_GDA2020";
                          } else {
                            fprintf(file_out, "GeographicTypeGeoKey: GCS_GDA2020\012");
                          }
                          break;
                        default:
                          if (json_out) {
                            char buffer[256];
                            snprintf(buffer, sizeof(buffer), "look-up for %d not implemented", lasreader->header.vlr_geo_key_entries[j].value_offset);
                            json_geo_key_entry["geographic_type_geo_key"] = buffer;
                          } else {
                            fprintf(
                                file_out, "GeographicTypeGeoKey: look-up for %d not implemented\012",
                                lasreader->header.vlr_geo_key_entries[j].value_offset);
                          }
                      }
                      break;
                    case 2049:  // GeogCitationGeoKey
                      if (lasreader->header.vlr_geo_ascii_params) {
                        char dummy[256];
                        strncpy_las(
                            dummy, sizeof(dummy), &(lasreader->header.vlr_geo_ascii_params[lasreader->header.vlr_geo_key_entries[j].value_offset]),
                            lasreader->header.vlr_geo_key_entries[j].count);
                        dummy[lasreader->header.vlr_geo_key_entries[j].count - 1] = '\0';
                        if (json_out) {
                          json_geo_key_entry["geog_citation_geo_key"] = dummy;
                        } else {
                          fprintf(file_out, "GeogCitationGeoKey: %s\012", dummy);
                        }
                      }
                      break;
                    case 2050:  // GeogGeodeticDatumGeoKey
                      switch (lasreader->header.vlr_geo_key_entries[j].value_offset) {
                        case 32767:  // user-defined
                          if (json_out) {
                            json_geo_key_entry["geog_geodetic_datum_geo_key"] = "user-defined";
                          } else {
                            fprintf(file_out, "GeogGeodeticDatumGeoKey: user-defined\012");
                          }
                          break;
                        case 6202:  // Datum_Australian_Geodetic_Datum_1966
                          if (json_out) {
                            json_geo_key_entry["geog_geodetic_datum_geo_key"] = "Datum_Australian_Geodetic_Datum_1966";
                          } else {
                            fprintf(file_out, "GeogGeodeticDatumGeoKey: Datum_Australian_Geodetic_Datum_1966\012");
                          }
                          break;
                        case 6203:  // Datum_Australian_Geodetic_Datum_1984
                          if (json_out) {
                            json_geo_key_entry["geog_geodetic_datum_geo_key"] = "Datum_Australian_Geodetic_Datum_1984";
                          } else {
                            fprintf(file_out, "GeogGeodeticDatumGeoKey: Datum_Australian_Geodetic_Datum_1984\012");
                          }
                          break;
                        case 6267:  // Datum_North_American_Datum_1927
                          if (json_out) {
                            json_geo_key_entry["geog_geodetic_datum_geo_key"] = "Datum_North_American_Datum_1927";
                          } else {
                            fprintf(file_out, "GeogGeodeticDatumGeoKey: Datum_North_American_Datum_1927\012");
                          }
                          break;
                        case 6269:  // Datum_North_American_Datum_1983
                          if (json_out) {
                            json_geo_key_entry["geog_geodetic_datum_geo_key"] = "Datum_North_American_Datum_1983";
                          } else {
                            fprintf(file_out, "GeogGeodeticDatumGeoKey: Datum_North_American_Datum_1983\012");
                          }
                          break;
                        case 6283:  // Datum_Geocentric_Datum_of_Australia_1994
                          if (json_out) {
                            json_geo_key_entry["geog_geodetic_datum_geo_key"] = "Datum_Geocentric_Datum_of_Australia_1994";
                          } else {
                            fprintf(file_out, "GeogGeodeticDatumGeoKey: Datum_Geocentric_Datum_of_Australia_1994\012");
                          }
                          break;
                        case 6322:  // Datum_WGS72
                          if (json_out) {
                            json_geo_key_entry["geog_geodetic_datum_geo_key"] = "Datum_WGS72";
                          } else {
                            fprintf(file_out, "GeogGeodeticDatumGeoKey: Datum_WGS72\012");
                          }
                          break;
                        case 6326:  // Datum_WGS84
                          if (json_out) {
                            json_geo_key_entry["geog_geodetic_datum_geo_key"] = "Datum_WGS84";
                          } else {
                            fprintf(file_out, "GeogGeodeticDatumGeoKey: Datum_WGS84\012");
                          }
                          break;
                        case 6140:  // Datum_WGS84
                          if (json_out) {
                            json_geo_key_entry["geog_geodetic_datum_geo_key"] = "Datum_NAD83_CSRS";
                          } else {
                            fprintf(file_out, "GeogGeodeticDatumGeoKey: Datum_NAD83_CSRS\012");
                          }
                          break;
                        case 6619:  // Datum_SWEREF99
                          if (json_out) {
                            json_geo_key_entry["geog_geodetic_datum_geo_key"] = "Datum_SWEREF99";
                          } else {
                            fprintf(file_out, "GeogGeodeticDatumGeoKey: Datum_SWEREF99\012");
                          }
                          break;
                        case 6289:  // Datum_Amersfoort
                          if (json_out) {
                            json_geo_key_entry["geog_geodetic_datum_geo_key"] = "Datum_Amersfoort";
                          } else {
                            fprintf(file_out, "GeogGeodeticDatumGeoKey: Datum_Amersfoort\012");
                          }
                          break;
                        case 6167:  // Datum_NZGD2000
                          if (json_out) {
                            json_geo_key_entry["geog_geodetic_datum_geo_key"] = "Datum_NZGD2000";
                          } else {
                            fprintf(file_out, "GeogGeodeticDatumGeoKey: Datum_NZGD2000\012");
                          }
                          break;
                        case 6001:  // DatumE_Airy1830
                          if (json_out) {
                            json_geo_key_entry["geog_geodetic_datum_geo_key"] = "DatumE_Airy1830";
                          } else {
                            fprintf(file_out, "GeogGeodeticDatumGeoKey: DatumE_Airy1830\012");
                          }
                          break;
                        case 6002:  // DatumE_AiryModified1849
                          if (json_out) {
                            json_geo_key_entry["geog_geodetic_datum_geo_key"] = "DatumE_AiryModified1849";
                          } else {
                            fprintf(file_out, "GeogGeodeticDatumGeoKey: DatumE_AiryModified1849\012");
                          }
                          break;
                        case 6003:  // DatumE_AustralianNationalSpheroid
                          if (json_out) {
                            json_geo_key_entry["geog_geodetic_datum_geo_key"] = "DatumE_AustralianNationalSpheroid";
                          } else {
                            fprintf(file_out, "GeogGeodeticDatumGeoKey: DatumE_AustralianNationalSpheroid\012");
                          }
                          break;
                        case 6004:  // DatumE_Bessel1841
                          if (json_out) {
                            json_geo_key_entry["geog_geodetic_datum_geo_key"] = "DatumE_Bessel1841";
                          } else {
                            fprintf(file_out, "GeogGeodeticDatumGeoKey: DatumE_Bessel1841\012");
                          }
                          break;
                        case 6005:  // DatumE_BesselModified
                          if (json_out) {
                            json_geo_key_entry["geog_geodetic_datum_geo_key"] = "DatumE_BesselModified";
                          } else {
                            fprintf(file_out, "GeogGeodeticDatumGeoKey: DatumE_BesselModified\012");
                          }
                          break;
                        case 6006:  // DatumE_BesselNamibia
                          if (json_out) {
                            json_geo_key_entry["geog_geodetic_datum_geo_key"] = "DatumE_BesselNamibia";
                          } else {
                            fprintf(file_out, "GeogGeodeticDatumGeoKey: DatumE_BesselNamibia\012");
                          }
                          break;
                        case 6008:  // DatumE_Clarke1866
                          if (json_out) {
                            json_geo_key_entry["geog_geodetic_datum_geo_key"] = "DatumE_Clarke1866";
                          } else {
                            fprintf(file_out, "GeogGeodeticDatumGeoKey: DatumE_Clarke1866\012");
                          }
                          break;
                        case 6009:  // DatumE_Clarke1866Michigan
                          if (json_out) {
                            json_geo_key_entry["geog_geodetic_datum_geo_key"] = "DatumE_Clarke1866Michigan";
                          } else {
                            fprintf(file_out, "GeogGeodeticDatumGeoKey: DatumE_Clarke1866Michigan\012");
                          }
                          break;
                        case 6010:  // DatumE_Clarke1880_Benoit
                          if (json_out) {
                            json_geo_key_entry["geog_geodetic_datum_geo_key"] = "DatumE_Clarke1880_Benoit";
                          } else {
                            fprintf(file_out, "GeogGeodeticDatumGeoKey: DatumE_Clarke1880_Benoit\012");
                          }
                          break;
                        case 6011:  // DatumE_Clarke1880_IGN
                          if (json_out) {
                            json_geo_key_entry["geog_geodetic_datum_geo_key"] = "DatumE_Clarke1880_IGN";
                          } else {
                            fprintf(file_out, "GeogGeodeticDatumGeoKey: DatumE_Clarke1880_IGN\012");
                          }
                          break;
                        case 6012:  // DatumE_Clarke1880_RGS
                          if (json_out) {
                            json_geo_key_entry["geog_geodetic_datum_geo_key"] = "DatumE_Clarke1880_RGS";
                          } else {
                            fprintf(file_out, "GeogGeodeticDatumGeoKey: DatumE_Clarke1880_RGS\012");
                          }
                          break;
                        case 6013:  // DatumE_Clarke1880_Arc
                          if (json_out) {
                            json_geo_key_entry["geog_geodetic_datum_geo_key"] = "DatumE_Clarke1880_Arc";
                          } else {
                            fprintf(file_out, "GeogGeodeticDatumGeoKey: DatumE_Clarke1880_Arc\012");
                          }
                          break;
                        case 6014:  // DatumE_Clarke1880_SGA1922
                          if (json_out) {
                            json_geo_key_entry["geog_geodetic_datum_geo_key"] = "DatumE_Clarke1880_SGA1922";
                          } else {
                            fprintf(file_out, "GeogGeodeticDatumGeoKey: DatumE_Clarke1880_SGA1922\012");
                          }
                          break;
                        case 6015:  // DatumE_Everest1830_1937Adjustment
                          if (json_out) {
                            json_geo_key_entry["geog_geodetic_datum_geo_key"] = "DatumE_Everest1830_1937Adjustment";
                          } else {
                            fprintf(file_out, "GeogGeodeticDatumGeoKey: DatumE_Everest1830_1937Adjustment\012");
                          }
                          break;
                        case 6016:  // DatumE_Everest1830_1967Definition
                          if (json_out) {
                            json_geo_key_entry["geog_geodetic_datum_geo_key"] = "DatumE_Everest1830_1967Definition";
                          } else {
                            fprintf(file_out, "GeogGeodeticDatumGeoKey: DatumE_Everest1830_1967Definition\012");
                          }
                          break;
                        case 6017:  // DatumE_Everest1830_1975Definition
                          if (json_out) {
                            json_geo_key_entry["geog_geodetic_datum_geo_key"] = "DatumE_Everest1830_1975Definition";
                          } else {
                            fprintf(file_out, "GeogGeodeticDatumGeoKey: DatumE_Everest1830_1975Definition\012");
                          }
                          break;
                        case 6018:  // DatumE_Everest1830Modified
                          if (json_out) {
                            json_geo_key_entry["geog_geodetic_datum_geo_key"] = "DatumE_Everest1830Modified";
                          } else {
                            fprintf(file_out, "GeogGeodeticDatumGeoKey: DatumE_Everest1830Modified\012");
                          }
                          break;
                        case 6019:  // DatumE_GRS1980
                          if (json_out) {
                            json_geo_key_entry["geog_geodetic_datum_geo_key"] = "DatumE_GRS1980";
                          } else {
                            fprintf(file_out, "GeogGeodeticDatumGeoKey: DatumE_GRS1980\012");
                          }
                          break;
                        case 6020:  // DatumE_Helmert1906
                          if (json_out) {
                            json_geo_key_entry["geog_geodetic_datum_geo_key"] = "DatumE_Helmert1906";
                          } else {
                            fprintf(file_out, "GeogGeodeticDatumGeoKey: DatumE_Helmert1906\012");
                          }
                          break;
                        case 6022:  // DatumE_International1924
                          if (json_out) {
                            json_geo_key_entry["geog_geodetic_datum_geo_key"] = "DatumE_International1924";
                          } else {
                            fprintf(file_out, "GeogGeodeticDatumGeoKey: DatumE_International1924\012");
                          }
                          break;
                        case 6023:  // DatumE_International1967
                          if (json_out) {
                            json_geo_key_entry["geog_geodetic_datum_geo_key"] = "DatumE_International1967";
                          } else {
                            fprintf(file_out, "GeogGeodeticDatumGeoKey: DatumE_International1967\012");
                          }
                          break;
                        case 6024:  // DatumE_Krassowsky1940
                          if (json_out) {
                            json_geo_key_entry["geog_geodetic_datum_geo_key"] = "DatumE_Krassowsky1940";
                          } else {
                            fprintf(file_out, "GeogGeodeticDatumGeoKey: DatumE_Krassowsky1940\012");
                          }
                          break;
                        case 6030:  // DatumE_WGS84
                          if (json_out) {
                            json_geo_key_entry["geog_geodetic_datum_geo_key"] = "DatumE_WGS84";
                          } else {
                            fprintf(file_out, "GeogGeodeticDatumGeoKey: DatumE_WGS84\012");
                          }
                          break;
                        case 6034:  // DatumE_Clarke1880
                          if (json_out) {
                            json_geo_key_entry["geog_geodetic_datum_geo_key"] = "DatumE_Clarke1880";
                          } else {
                            fprintf(file_out, "GeogGeodeticDatumGeoKey: DatumE_Clarke1880\012");
                          }
                          break;
                        default:
                          if (json_out) {
                            char buffer[256];
                            snprintf(buffer, sizeof(buffer), "look-up for %d not implemented", lasreader->header.vlr_geo_key_entries[j].value_offset);
                            json_geo_key_entry["geog_geodetic_datum_geo_key"] = buffer;
                          } else {
                            fprintf(
                                file_out, "GeogGeodeticDatumGeoKey: look-up for %d not implemented\012",
                                lasreader->header.vlr_geo_key_entries[j].value_offset);
                          }
                      }
                      break;
                    case 2051:  // GeogPrimeMeridianGeoKey
                      switch (lasreader->header.vlr_geo_key_entries[j].value_offset) {
                        case 32767:  // user-defined
                          if (json_out) {
                            json_geo_key_entry["geog_prime_meridian_geo_key"] = "user-defined";
                          } else {
                            fprintf(file_out, "GeogPrimeMeridianGeoKey: user-defined\012");
                          }
                          break;
                        case 8901:  // PM_Greenwich
                          if (json_out) {
                            json_geo_key_entry["geog_prime_meridian_geo_key"] = "PM_Greenwich";
                          } else {
                            fprintf(file_out, "GeogPrimeMeridianGeoKey: PM_Greenwich\012");
                          }
                          break;
                        case 8902:  // PM_Lisbon
                          if (json_out) {
                            json_geo_key_entry["geog_prime_meridian_geo_key"] = "PM_Lisbon";
                          } else {
                            fprintf(file_out, "GeogPrimeMeridianGeoKey: PM_Lisbon\012");
                          }
                          break;
                        default:
                          if (json_out) {
                            char buffer[256];
                            snprintf(buffer, sizeof(buffer), "look-up for %d not implemented", lasreader->header.vlr_geo_key_entries[j].value_offset);
                            json_geo_key_entry["geog_prime_meridian_geo_key"] = buffer;
                          } else {
                            fprintf(
                                file_out, "GeogPrimeMeridianGeoKey: look-up for %d not implemented\012",
                                lasreader->header.vlr_geo_key_entries[j].value_offset);
                          }
                      }
                      break;
                    case 2052:  // GeogLinearUnitsGeoKey
                      horizontal_units = lasreader->header.vlr_geo_key_entries[j].value_offset;
                      switch (lasreader->header.vlr_geo_key_entries[j].value_offset) {
                        case 9001:  // Linear_Meter
                          if (json_out) {
                            json_geo_key_entry["geog_linear_units_geo_key"] = "Linear_Meter";
                          } else {
                            fprintf(file_out, "GeogLinearUnitsGeoKey: Linear_Meter\012");
                          }
                          break;
                        case 9002:  // Linear_Foot
                          if (json_out) {
                            json_geo_key_entry["geog_linear_units_geo_key"] = "Linear_Foot";
                          } else {
                            fprintf(file_out, "GeogLinearUnitsGeoKey: Linear_Foot\012");
                          }
                          break;
                        case 9003:  // Linear_Foot_US_Survey
                          if (json_out) {
                            json_geo_key_entry["geog_linear_units_geo_key"] = "Linear_Foot_US_Survey";
                          } else {
                            fprintf(file_out, "GeogLinearUnitsGeoKey: Linear_Foot_US_Survey\012");
                          }
                          break;
                        case 9004:  // Linear_Foot_Modified_American
                          if (json_out) {
                            json_geo_key_entry["geog_linear_units_geo_key"] = "Linear_Foot_Modified_American";
                          } else {
                            fprintf(file_out, "GeogLinearUnitsGeoKey: Linear_Foot_Modified_American\012");
                          }
                          break;
                        case 9005:  // Linear_Foot_Clarke
                          if (json_out) {
                            json_geo_key_entry["geog_linear_units_geo_key"] = "Linear_Foot_Clarke";
                          } else {
                            fprintf(file_out, "GeogLinearUnitsGeoKey: Linear_Foot_Clarke\012");
                          }
                          break;
                        case 9006:  // Linear_Foot_Indian
                          if (json_out) {
                            json_geo_key_entry["geog_linear_units_geo_key"] = "Linear_Foot_Indian";
                          } else {
                            fprintf(file_out, "GeogLinearUnitsGeoKey: Linear_Foot_Indian\012");
                          }
                          break;
                        case 9007:  // Linear_Link
                          if (json_out) {
                            json_geo_key_entry["geog_linear_units_geo_key"] = "Linear_Link";
                          } else {
                            fprintf(file_out, "GeogLinearUnitsGeoKey: Linear_Link\012");
                          }
                          break;
                        case 9008:  // Linear_Link_Benoit
                          if (json_out) {
                            json_geo_key_entry["geog_linear_units_geo_key"] = "Linear_Link_Benoit";
                          } else {
                            fprintf(file_out, "GeogLinearUnitsGeoKey: Linear_Link_Benoit\012");
                          }
                          break;
                        case 9009:  // Linear_Link_Sears
                          if (json_out) {
                            json_geo_key_entry["geog_linear_units_geo_key"] = "Linear_Link_Sears";
                          } else {
                            fprintf(file_out, "GeogLinearUnitsGeoKey: Linear_Link_Sears\012");
                          }
                          break;
                        case 9010:  // Linear_Chain_Benoit
                          if (json_out) {
                            json_geo_key_entry["geog_linear_units_geo_key"] = "Linear_Chain_Benoit";
                          } else {
                            fprintf(file_out, "GeogLinearUnitsGeoKey: Linear_Chain_Benoit\012");
                          }
                          break;
                        case 9011:  // Linear_Chain_Sears
                          if (json_out) {
                            json_geo_key_entry["geog_linear_units_geo_key"] = "Linear_Chain_Sears";
                          } else {
                            fprintf(file_out, "GeogLinearUnitsGeoKey: Linear_Chain_Sears\012");
                          }
                          break;
                        case 9012:  // Linear_Yard_Sears
                          if (json_out) {
                            json_geo_key_entry["geog_linear_units_geo_key"] = "Linear_Yard_Sears";
                          } else {
                            fprintf(file_out, "GeogLinearUnitsGeoKey: Linear_Yard_Sears\012");
                          }
                          break;
                        case 9013:  // Linear_Yard_Indian
                          if (json_out) {
                            json_geo_key_entry["geog_linear_units_geo_key"] = "Linear_Yard_Indian";
                          } else {
                            fprintf(file_out, "GeogLinearUnitsGeoKey: Linear_Yard_Indian\012");
                          }
                          break;
                        case 9014:  // Linear_Fathom
                          if (json_out) {
                            json_geo_key_entry["geog_linear_units_geo_key"] = "Linear_Fathom";
                          } else {
                            fprintf(file_out, "GeogLinearUnitsGeoKey: Linear_Fathom\012");
                          }
                          break;
                        case 9015:  // Linear_Mile_International_Nautical
                          if (json_out) {
                            json_geo_key_entry["geog_linear_units_geo_key"] = "Linear_Mile_International_Nautical";
                          } else {
                            fprintf(file_out, "GeogLinearUnitsGeoKey: Linear_Mile_International_Nautical\012");
                          }
                          break;
                        default:
                          if (json_out) {
                            char buffer[256];
                            snprintf(buffer, sizeof(buffer), "look-up for %d not implemented", lasreader->header.vlr_geo_key_entries[j].value_offset);
                            json_geo_key_entry["geog_linear_units_geo_key"] = buffer;
                          } else {
                            fprintf(
                                file_out, "GeogLinearUnitsGeoKey: look-up for %d not implemented\012",
                                lasreader->header.vlr_geo_key_entries[j].value_offset);
                          }
                      }
                      break;
                    case 2053:  // GeogLinearUnitSizeGeoKey
                      if (lasreader->header.vlr_geo_double_params) {
                        if (json_out) {
                          json_geo_key_entry["geog_linear_unit_size_geo_key"] =
                              round_to_decimals(lasreader->header.vlr_geo_double_params[lasreader->header.vlr_geo_key_entries[j].value_offset], 10);
                        } else {
                          fprintf(
                              file_out, "GeogLinearUnitSizeGeoKey: %.10g\012",
                              lasreader->header.vlr_geo_double_params[lasreader->header.vlr_geo_key_entries[j].value_offset]);
                        }
                      }
                      break;
                    case 2054:  // GeogAngularUnitsGeoKey
                      switch (lasreader->header.vlr_geo_key_entries[j].value_offset) {
                        case 9101:  // Angular_Radian
                          if (json_out) {
                            json_geo_key_entry["geog_angular_units_geo_key"] = "Angular_Radian";
                          } else {
                            fprintf(file_out, "GeogAngularUnitsGeoKey: Angular_Radian\012");
                          }
                          break;
                        case 9102:  // Angular_Degree
                          if (json_out) {
                            json_geo_key_entry["geog_angular_units_geo_key"] = "Angular_Degree";
                          } else {
                            fprintf(file_out, "GeogAngularUnitsGeoKey: Angular_Degree\012");
                          }
                          break;
                        case 9103:  // Angular_Arc_Minute
                          if (json_out) {
                            json_geo_key_entry["geog_angular_units_geo_key"] = "Angular_Arc_Minute";
                          } else {
                            fprintf(file_out, "GeogAngularUnitsGeoKey: Angular_Arc_Minute\012");
                          }
                          break;
                        case 9104:  // Angular_Arc_Second
                          if (json_out) {
                            json_geo_key_entry["geog_angular_units_geo_key"] = "Angular_Arc_Second";
                          } else {
                            fprintf(file_out, "GeogAngularUnitsGeoKey: Angular_Arc_Second\012");
                          }
                          break;
                        case 9105:  // Angular_Grad
                          if (json_out) {
                            json_geo_key_entry["geog_angular_units_geo_key"] = "Angular_Grad";
                          } else {
                            fprintf(file_out, "GeogAngularUnitsGeoKey: Angular_Grad\012");
                          }
                          break;
                        case 9106:  // Angular_Gon
                          if (json_out) {
                            json_geo_key_entry["geog_angular_units_geo_key"] = "Angular_Gon";
                          } else {
                            fprintf(file_out, "GeogAngularUnitsGeoKey: Angular_Gon\012");
                          }
                          break;
                        case 9107:  // Angular_DMS
                          if (json_out) {
                            json_geo_key_entry["geog_angular_units_geo_key"] = "Angular_DMS";
                          } else {
                            fprintf(file_out, "GeogAngularUnitsGeoKey: Angular_DMS\012");
                          }
                          break;
                        case 9108:  // Angular_DMS_Hemisphere
                          if (json_out) {
                            json_geo_key_entry["geog_angular_units_geo_key"] = "Angular_DMS_Hemisphere";
                          } else {
                            fprintf(file_out, "GeogAngularUnitsGeoKey: Angular_DMS_Hemisphere\012");
                          }
                          break;
                        default:
                          if (json_out) {
                            char buffer[256];
                            snprintf(buffer, sizeof(buffer), "look-up for %d not implemented", lasreader->header.vlr_geo_key_entries[j].value_offset);
                            json_geo_key_entry["geog_angular_units_geo_key"] = buffer;
                          } else {
                            fprintf(
                                file_out, "GeogAngularUnitsGeoKey: look-up for %d not implemented\012",
                                lasreader->header.vlr_geo_key_entries[j].value_offset);
                          }
                      }
                      break;
                    case 2055:  // GeogAngularUnitSizeGeoKey
                      if (lasreader->header.vlr_geo_double_params) {
                        if (json_out) {
                          json_geo_key_entry["geog_angular_unit_size_geo_key"] =
                              round_to_decimals(lasreader->header.vlr_geo_double_params[lasreader->header.vlr_geo_key_entries[j].value_offset], 10);
                        } else {
                          fprintf(
                              file_out, "GeogAngularUnitSizeGeoKey: %.10g\012",
                              lasreader->header.vlr_geo_double_params[lasreader->header.vlr_geo_key_entries[j].value_offset]);
                        }
                      }
                      break;
                    case 2056:  // GeogEllipsoidGeoKey
                      switch (lasreader->header.vlr_geo_key_entries[j].value_offset) {
                        case 32767:  // user-defined
                          if (json_out) {
                            json_geo_key_entry["geog_ellipsoid_geo_key"] = "user-defined";
                          } else {
                            fprintf(file_out, "GeogEllipsoidGeoKey: user-defined\012");
                          }
                          break;
                        case 7001:  // Ellipse_Airy_1830
                          if (json_out) {
                            json_geo_key_entry["geog_ellipsoid_geo_key"] = "Ellipse_Airy_1830";
                          } else {
                            fprintf(file_out, "GeogEllipsoidGeoKey: Ellipse_Airy_1830\012");
                          }
                          break;
                        case 7002:  // Ellipse_Airy_Modified_1849
                          if (json_out) {
                            json_geo_key_entry["geog_ellipsoid_geo_key"] = "Ellipse_Airy_Modified_1849";
                          } else {
                            fprintf(file_out, "GeogEllipsoidGeoKey: Ellipse_Airy_Modified_1849\012");
                          }
                          break;
                        case 7003:  // Ellipse_Australian_National_Spheroid
                          if (json_out) {
                            json_geo_key_entry["geog_ellipsoid_geo_key"] = "Ellipse_Australian_National_Spheroid";
                          } else {
                            fprintf(file_out, "GeogEllipsoidGeoKey: Ellipse_Australian_National_Spheroid\012");
                          }
                          break;
                        case 7004:  // Ellipse_Bessel_1841
                          if (json_out) {
                            json_geo_key_entry["geog_ellipsoid_geo_key"] = "Ellipse_Bessel_1841";
                          } else {
                            fprintf(file_out, "GeogEllipsoidGeoKey: Ellipse_Bessel_1841\012");
                          }
                          break;
                        case 7005:  // Ellipse_Bessel_Modified
                          if (json_out) {
                            json_geo_key_entry["geog_ellipsoid_geo_key"] = "Ellipse_Bessel_Modified";
                          } else {
                            fprintf(file_out, "GeogEllipsoidGeoKey: Ellipse_Bessel_Modified\012");
                          }
                          break;
                        case 7006:  // Ellipse_Bessel_Namibia
                          if (json_out) {
                            json_geo_key_entry["geog_ellipsoid_geo_key"] = "Ellipse_Bessel_Namibia";
                          } else {
                            fprintf(file_out, "GeogEllipsoidGeoKey: Ellipse_Bessel_Namibia\012");
                          }
                          break;
                        case 7008:  // Ellipse_Clarke_1866
                          if (json_out) {
                            json_geo_key_entry["geog_ellipsoid_geo_key"] = "Ellipse_Clarke_1866";
                          } else {
                            fprintf(file_out, "GeogEllipsoidGeoKey: Ellipse_Clarke_1866\012");
                          }
                          break;
                        case 7009:  // Ellipse_Clarke_1866_Michigan
                          if (json_out) {
                            json_geo_key_entry["geog_ellipsoid_geo_key"] = "Ellipse_Clarke_1866_Michigan";
                          } else {
                            fprintf(file_out, "GeogEllipsoidGeoKey: Ellipse_Clarke_1866_Michigan\012");
                          }
                          break;
                        case 7010:  // Ellipse_Clarke1880_Benoit
                          if (json_out) {
                            json_geo_key_entry["geog_ellipsoid_geo_key"] = "Ellipse_Clarke1880_Benoit";
                          } else {
                            fprintf(file_out, "GeogEllipsoidGeoKey: Ellipse_Clarke1880_Benoit\012");
                          }
                          break;
                        case 7011:  // Ellipse_Clarke1880_IGN
                          if (json_out) {
                            json_geo_key_entry["geog_ellipsoid_geo_key"] = "Ellipse_Clarke1880_IGN";
                          } else {
                            fprintf(file_out, "GeogEllipsoidGeoKey: Ellipse_Clarke1880_IGN\012");
                          }
                          break;
                        case 7012:  // Ellipse_Clarke1880_RGS
                          if (json_out) {
                            json_geo_key_entry["geog_ellipsoid_geo_key"] = "Ellipse_Clarke1880_RGS";
                          } else {
                            fprintf(file_out, "GeogEllipsoidGeoKey: Ellipse_Clarke1880_RGS\012");
                          }
                          break;
                        case 7013:  // Ellipse_Clarke1880_Arc
                          if (json_out) {
                            json_geo_key_entry["geog_ellipsoid_geo_key"] = "Ellipse_Clarke1880_Arc";
                          } else {
                            fprintf(file_out, "GeogEllipsoidGeoKey: Ellipse_Clarke1880_Arc\012");
                          }
                          break;
                        case 7014:  // Ellipse_Clarke1880_SGA1922
                          if (json_out) {
                            json_geo_key_entry["geog_ellipsoid_geo_key"] = "Ellipse_Clarke1880_SGA1922";
                          } else {
                            fprintf(file_out, "GeogEllipsoidGeoKey: Ellipse_Clarke1880_SGA1922\012");
                          }
                          break;
                        case 7015:  // Ellipse_Everest1830_1937Adjustment
                          if (json_out) {
                            json_geo_key_entry["geog_ellipsoid_geo_key"] = "Ellipse_Everest1830_1937Adjustment";
                          } else {
                            fprintf(file_out, "GeogEllipsoidGeoKey: Ellipse_Everest1830_1937Adjustment\012");
                          }
                          break;
                        case 7016:  // Ellipse_Everest1830_1967Definition
                          if (json_out) {
                            json_geo_key_entry["geog_ellipsoid_geo_key"] = "Ellipse_Everest1830_1967Definition";
                          } else {
                            fprintf(file_out, "GeogEllipsoidGeoKey: Ellipse_Everest1830_1967Definition\012");
                          }
                          break;
                        case 7017:  // Ellipse_Everest1830_1975Definition
                          if (json_out) {
                            json_geo_key_entry["geog_ellipsoid_geo_key"] = "Ellipse_Everest1830_1975Definition";
                          } else {
                            fprintf(file_out, "GeogEllipsoidGeoKey: Ellipse_Everest1830_1975Definition\012");
                          }
                          break;
                        case 7018:  // Ellipse_Everest1830Modified
                          if (json_out) {
                            json_geo_key_entry["geog_ellipsoid_geo_key"] = "Ellipse_Everest1830Modified";
                          } else {
                            fprintf(file_out, "GeogEllipsoidGeoKey: Ellipse_Everest1830Modified\012");
                          }
                          break;
                        case 7019:  // Ellipse_GRS_1980
                          if (json_out) {
                            json_geo_key_entry["geog_ellipsoid_geo_key"] = "Ellipse_GRS_1980";
                          } else {
                            fprintf(file_out, "GeogEllipsoidGeoKey: Ellipse_GRS_1980\012");
                          }
                          break;
                        case 7020:  // Ellipse_Helmert1906
                          if (json_out) {
                            json_geo_key_entry["geog_ellipsoid_geo_key"] = "Ellipse_Helmert1906";
                          } else {
                            fprintf(file_out, "GeogEllipsoidGeoKey: Ellipse_Helmert1906\012");
                          }
                          break;
                        case 7022:  // Ellipse_International1924
                          if (json_out) {
                            json_geo_key_entry["geog_ellipsoid_geo_key"] = "Ellipse_International1924";
                          } else {
                            fprintf(file_out, "GeogEllipsoidGeoKey: Ellipse_International1924\012");
                          }
                          break;
                        case 7023:  // Ellipse_International1967
                          if (json_out) {
                            json_geo_key_entry["geog_ellipsoid_geo_key"] = "Ellipse_International1967";
                          } else {
                            fprintf(file_out, "GeogEllipsoidGeoKey: Ellipse_International1967\012");
                          }
                          break;
                        case 7024:  // Ellipse_Krassowsky1940
                          if (json_out) {
                            json_geo_key_entry["geog_ellipsoid_geo_key"] = "Ellipse_Krassowsky1940";
                          } else {
                            fprintf(file_out, "GeogEllipsoidGeoKey: Ellipse_Krassowsky1940\012");
                          }
                          break;
                        case 7030:  // Ellipse_WGS_84
                          if (json_out) {
                            json_geo_key_entry["geog_ellipsoid_geo_key"] = "Ellipse_WGS_84";
                          } else {
                            fprintf(file_out, "GeogEllipsoidGeoKey: Ellipse_WGS_84\012");
                          }
                          break;
                        case 7034:  // Ellipse_Clarke_1880
                          if (json_out) {
                            json_geo_key_entry["geog_ellipsoid_geo_key"] = "Ellipse_Clarke_1880";
                          } else {
                            fprintf(file_out, "GeogEllipsoidGeoKey: Ellipse_Clarke_1880\012");
                          }
                          break;
                        default:
                          if (json_out) {
                            char buffer[256];
                            snprintf(buffer, sizeof(buffer), "look-up for %d not implemented", lasreader->header.vlr_geo_key_entries[j].value_offset);
                            json_geo_key_entry["geog_ellipsoid_geo_key"] = buffer;
                          } else {
                            fprintf(
                                file_out, "GeogEllipsoidGeoKey: look-up for %d not implemented\012",
                                lasreader->header.vlr_geo_key_entries[j].value_offset);
                          }
                      }
                      break;
                    case 2057:  // GeogSemiMajorAxisGeoKey
                      if (lasreader->header.vlr_geo_double_params) {
                        if (json_out) {
                          json_geo_key_entry["geog_semi_major_axis_geo_key"] =
                              round_to_decimals(lasreader->header.vlr_geo_double_params[lasreader->header.vlr_geo_key_entries[j].value_offset], 10);
                        } else if (lasreader->header.vlr_geo_double_params) {
                          fprintf(
                              file_out, "GeogSemiMajorAxisGeoKey: %.10g\012",
                              lasreader->header.vlr_geo_double_params[lasreader->header.vlr_geo_key_entries[j].value_offset]);
                        }
                      }
                      break;
                    case 2058:  // GeogSemiMinorAxisGeoKey
                      if (lasreader->header.vlr_geo_double_params) {
                        if (json_out) {
                          json_geo_key_entry["geog_semi_minor_axis_geo_key"] =
                              round_to_decimals(lasreader->header.vlr_geo_double_params[lasreader->header.vlr_geo_key_entries[j].value_offset], 10);
                        } else if (lasreader->header.vlr_geo_double_params) {
                          fprintf(
                              file_out, "GeogSemiMinorAxisGeoKey: %.10g\012",
                              lasreader->header.vlr_geo_double_params[lasreader->header.vlr_geo_key_entries[j].value_offset]);
                        }
                      }
                      break;
                    case 2059:  // GeogInvFlatteningGeoKey
                      if (lasreader->header.vlr_geo_double_params) {
                        if (json_out) {
                          json_geo_key_entry["geog_inv_flattening_geo_key"] =
                              round_to_decimals(lasreader->header.vlr_geo_double_params[lasreader->header.vlr_geo_key_entries[j].value_offset], 10);
                        } else if (lasreader->header.vlr_geo_double_params) {
                          fprintf(
                              file_out, "GeogInvFlatteningGeoKey: %.10g\012",
                              lasreader->header.vlr_geo_double_params[lasreader->header.vlr_geo_key_entries[j].value_offset]);
                        }
                      }
                      break;
                    case 2060:  // GeogAzimuthUnitsGeoKey
                      switch (lasreader->header.vlr_geo_key_entries[j].value_offset) {
                        case 9101:  // Angular_Radian
                          if (json_out) {
                            json_geo_key_entry["geog_azimuth_units_geo_key"] = "Angular_Radian";
                          } else {
                            fprintf(file_out, "GeogAzimuthUnitsGeoKey: Angular_Radian\012");
                          }
                          break;
                        case 9102:  // Angular_Degree
                          if (json_out) {
                            json_geo_key_entry["geog_azimuth_units_geo_key"] = "Angular_Degree";
                          } else {
                            fprintf(file_out, "GeogAzimuthUnitsGeoKey: Angular_Degree\012");
                          }
                          break;
                        case 9103:  // Angular_Arc_Minute
                          if (json_out) {
                            json_geo_key_entry["geog_azimuth_units_geo_key"] = "Angular_Arc_Minute";
                          } else {
                            fprintf(file_out, "GeogAzimuthUnitsGeoKey: Angular_Arc_Minute\012");
                          }
                          break;
                        case 9104:  // Angular_Arc_Second
                          if (json_out) {
                            json_geo_key_entry["geog_azimuth_units_geo_key"] = "Angular_Arc_Second";
                          } else {
                            fprintf(file_out, "GeogAzimuthUnitsGeoKey: Angular_Arc_Second\012");
                          }
                          break;
                        case 9105:  // Angular_Grad
                          if (json_out) {
                            json_geo_key_entry["geog_azimuth_units_geo_key"] = "Angular_Arc_Second";
                          } else {
                            fprintf(file_out, "GeogAzimuthUnitsGeoKey: Angular_Arc_Second\012");
                          }
                          break;
                        case 9106:  // Angular_Gon
                          if (json_out) {
                            json_geo_key_entry["geog_azimuth_units_geo_key"] = "Angular_Gon";
                          } else {
                            fprintf(file_out, "GeogAzimuthUnitsGeoKey: Angular_Gon\012");
                          }
                          break;
                        case 9107:  // Angular_DMS
                          if (json_out) {
                            json_geo_key_entry["geog_azimuth_units_geo_key"] = "Angular_DMS";
                          } else {
                            fprintf(file_out, "GeogAzimuthUnitsGeoKey: Angular_DMS\012");
                          }
                          break;
                        case 9108:  // Angular_DMS_Hemisphere
                          if (json_out) {
                            json_geo_key_entry["geog_azimuth_units_geo_key"] = "Angular_DMS_Hemisphere";
                          } else {
                            fprintf(file_out, "GeogAzimuthUnitsGeoKey: Angular_DMS_Hemisphere\012");
                          }
                          break;
                        default:
                          if (json_out) {
                            char buffer[100];
                            snprintf(buffer, sizeof(buffer), "look-up for %d not implemented", lasreader->header.vlr_geo_key_entries[j].value_offset);
                            json_geo_key_entry["geog_azimuth_units_geo_key"] = buffer;
                          } else {
                            fprintf(
                                file_out, "GeogAzimuthUnitsGeoKey: look-up for %d not implemented\012",
                                lasreader->header.vlr_geo_key_entries[j].value_offset);
                          }
                      }
                      break;
                    case 2061:  // GeogPrimeMeridianLongGeoKey
                      if (lasreader->header.vlr_geo_double_params) {
                        if (json_out) {
                          json_geo_key_entry["geog_prime_meridian_long_geo_key"] =
                              round_to_decimals(lasreader->header.vlr_geo_double_params[lasreader->header.vlr_geo_key_entries[j].value_offset], 10);
                        } else if (lasreader->header.vlr_geo_double_params) {
                          fprintf(
                              file_out, "GeogPrimeMeridianLongGeoKey: %.10g\012",
                              lasreader->header.vlr_geo_double_params[lasreader->header.vlr_geo_key_entries[j].value_offset]);
                        }
                      }
                      break;
                    case 2062:  // GeogTOWGS84GeoKey
                      switch (lasreader->header.vlr_geo_key_entries[j].count) {
                        case 3:
                          if (lasreader->header.vlr_geo_double_params) {
                            if (json_out) {
                              json_geo_key_entry["geog_towgs84_geo_key"].push_back(round_to_decimals(
                                  lasreader->header.vlr_geo_double_params[lasreader->header.vlr_geo_key_entries[j].value_offset], 10));
                              json_geo_key_entry["geog_towgs84_geo_key"].push_back(round_to_decimals(
                                  lasreader->header.vlr_geo_double_params[lasreader->header.vlr_geo_key_entries[j].value_offset + 1], 10));
                              json_geo_key_entry["geog_towgs84_geo_key"].push_back(round_to_decimals(
                                  lasreader->header.vlr_geo_double_params[lasreader->header.vlr_geo_key_entries[j].value_offset + 2], 10));
                            } else {
                              fprintf(
                                  file_out, "GeogTOWGS84GeoKey: TOWGS84[%.10g,%.10g,%.10g]\012",
                                  lasreader->header.vlr_geo_double_params[lasreader->header.vlr_geo_key_entries[j].value_offset],
                                  lasreader->header.vlr_geo_double_params[lasreader->header.vlr_geo_key_entries[j].value_offset + 1],
                                  lasreader->header.vlr_geo_double_params[lasreader->header.vlr_geo_key_entries[j].value_offset + 2]);
                            }
                          } else {
                            if (json_out) {
                              json_geo_key_entry["geog_towgs84_geo_key"] = "no vlr_geo_double_params. cannot look up the three parameters.";
                            } else {
                              fprintf(file_out, "GeogTOWGS84GeoKey: no vlr_geo_double_params. cannot look up the three parameters.\012");
                            }
                          }
                          break;
                        case 7:
                          if (lasreader->header.vlr_geo_double_params) {
                            if (json_out) {
                              json_geo_key_entry["geog_towgs84_geo_key"].push_back(round_to_decimals(
                                  lasreader->header.vlr_geo_double_params[lasreader->header.vlr_geo_key_entries[j].value_offset], 10));
                              json_geo_key_entry["geog_towgs84_geo_key"].push_back(round_to_decimals(
                                  lasreader->header.vlr_geo_double_params[lasreader->header.vlr_geo_key_entries[j].value_offset + 1], 10));
                              json_geo_key_entry["geog_towgs84_geo_key"].push_back(round_to_decimals(
                                  lasreader->header.vlr_geo_double_params[lasreader->header.vlr_geo_key_entries[j].value_offset + 2], 10));
                              json_geo_key_entry["geog_towgs84_geo_key"].push_back(round_to_decimals(
                                  lasreader->header.vlr_geo_double_params[lasreader->header.vlr_geo_key_entries[j].value_offset + 3], 10));
                              json_geo_key_entry["geog_towgs84_geo_key"].push_back(round_to_decimals(
                                  lasreader->header.vlr_geo_double_params[lasreader->header.vlr_geo_key_entries[j].value_offset + 4], 10));
                              json_geo_key_entry["geog_towgs84_geo_key"].push_back(round_to_decimals(
                                  lasreader->header.vlr_geo_double_params[lasreader->header.vlr_geo_key_entries[j].value_offset + 5], 10));
                              json_geo_key_entry["geog_towgs84_geo_key"].push_back(round_to_decimals(
                                  lasreader->header.vlr_geo_double_params[lasreader->header.vlr_geo_key_entries[j].value_offset + 6], 10));
                            } else {
                              fprintf(
                                  file_out, "GeogTOWGS84GeoKey: TOWGS84[%.10g,%.10g,%.10g,%.10g,%.10g,%.10g,%.10g]\012",
                                  lasreader->header.vlr_geo_double_params[lasreader->header.vlr_geo_key_entries[j].value_offset],
                                  lasreader->header.vlr_geo_double_params[lasreader->header.vlr_geo_key_entries[j].value_offset + 1],
                                  lasreader->header.vlr_geo_double_params[lasreader->header.vlr_geo_key_entries[j].value_offset + 2],
                                  lasreader->header.vlr_geo_double_params[lasreader->header.vlr_geo_key_entries[j].value_offset + 3],
                                  lasreader->header.vlr_geo_double_params[lasreader->header.vlr_geo_key_entries[j].value_offset + 4],
                                  lasreader->header.vlr_geo_double_params[lasreader->header.vlr_geo_key_entries[j].value_offset + 5],
                                  lasreader->header.vlr_geo_double_params[lasreader->header.vlr_geo_key_entries[j].value_offset + 6]);
                            }
                          } else {
                            if (json_out) {
                              json_geo_key_entry["geog_towgs84_geo_key"] = "no vlr_geo_double_params. cannot look up the seven parameters.";
                            } else {
                              fprintf(file_out, "GeogTOWGS84GeoKey: no vlr_geo_double_params. cannot look up the seven parameters.\012");
                            }
                          }
                          break;
                        default:
                          if (json_out) {
                            char buffer[50];
                            snprintf(buffer, sizeof(buffer), "look-up for type %d not implemented", lasreader->header.vlr_geo_key_entries[j].count);
                            json_geo_key_entry["geog_towgs84_geo_key"] = buffer;
                          } else {
                            fprintf(
                                file_out, "GeogTOWGS84GeoKey: look-up for type %d not implemented\012",
                                lasreader->header.vlr_geo_key_entries[j].count);
                          }
                      }
                      break;
                    case 3072:  // ProjectedCSTypeGeoKey
                      if (geoprojectionconverter.set_ProjectedCSTypeGeoKey(lasreader->header.vlr_geo_key_entries[j].value_offset, printstring)) {
                        horizontal_units = geoprojectionconverter.get_ProjLinearUnitsGeoKey();
                        if (json_out) {
                          json_geo_key_entry["projected_cs_type_geo_key"] = printstring;
                        } else {
                          fprintf(file_out, "ProjectedCSTypeGeoKey: %s\012", printstring);
                        }
                        break;
                      } else {
                        if (json_out) {
                          char buffer[100];
                          snprintf(buffer, sizeof(buffer), "look-up for %d not implemented", lasreader->header.vlr_geo_key_entries[j].value_offset);
                          json_geo_key_entry["projected_cs_type_geo_key"] = buffer;
                        } else {
                          fprintf(
                              file_out, "ProjectedCSTypeGeoKey: look-up for %d not implemented\012",
                              lasreader->header.vlr_geo_key_entries[j].value_offset);
                        }
                      }
                      break;
                    case 3073:  // PCSCitationGeoKey
                      if (lasreader->header.vlr_geo_ascii_params) {
                        char dummy[256];
                        strncpy_las(
                            dummy, sizeof(dummy), &(lasreader->header.vlr_geo_ascii_params[lasreader->header.vlr_geo_key_entries[j].value_offset]),
                            lasreader->header.vlr_geo_key_entries[j].count);
                        dummy[lasreader->header.vlr_geo_key_entries[j].count - 1] = '\0';
                        if (json_out) {
                          json_geo_key_entry["pcs_citation_geo_key"] = dummy;
                        } else {
                          fprintf(file_out, "PCSCitationGeoKey: %s\012", dummy);
                        }
                      }
                      break;
                    case 3074:  // ProjectionGeoKey
                      if ((16001 <= lasreader->header.vlr_geo_key_entries[j].value_offset) &&
                          (lasreader->header.vlr_geo_key_entries[j].value_offset <= 16060)) {
                        if (json_out) {
                          char buffer[100];
                          snprintf(buffer, sizeof(buffer), "Proj_UTM_zone_%dN", lasreader->header.vlr_geo_key_entries[j].value_offset - 16000);
                          json_geo_key_entry["projection_geo_key"] = buffer;
                        } else {
                          fprintf(file_out, "ProjectionGeoKey: Proj_UTM_zone_%dN\012", lasreader->header.vlr_geo_key_entries[j].value_offset - 16000);
                        }
                      } else if (
                          (16101 <= lasreader->header.vlr_geo_key_entries[j].value_offset) &&
                          (lasreader->header.vlr_geo_key_entries[j].value_offset <= 16160)) {
                        if (json_out) {
                          char buffer[100];
                          snprintf(buffer, sizeof(buffer), "Proj_UTM_zone_%dS", lasreader->header.vlr_geo_key_entries[j].value_offset - 16100);
                          json_geo_key_entry["projection_geo_key"] = buffer;
                        } else {
                          fprintf(file_out, "ProjectionGeoKey: Proj_UTM_zone_%dS\012", lasreader->header.vlr_geo_key_entries[j].value_offset - 16100);
                        }
                      } else {
                        switch (lasreader->header.vlr_geo_key_entries[j].value_offset) {
                          case 32767:  // user-defined
                            if (json_out) {
                              json_geo_key_entry["projection_geo_key"] = "user-defined";
                            } else {
                              fprintf(file_out, "ProjectionGeoKey: user-defined\012");
                            }
                            break;
                          case 10101:  // Proj_Alabama_CS27_East
                            if (json_out) {
                              json_geo_key_entry["projection_geo_key"] = "Proj_Alabama_CS27_East";
                            } else {
                              fprintf(file_out, "ProjectionGeoKey: Proj_Alabama_CS27_East\012");
                            }
                            break;
                          case 10102:  // Proj_Alabama_CS27_West
                            if (json_out) {
                              json_geo_key_entry["projection_geo_key"] = "Proj_Alabama_CS27_West";
                            } else {
                              fprintf(file_out, "ProjectionGeoKey: Proj_Alabama_CS27_West\012");
                            }
                            break;
                          case 10131:  // Proj_Alabama_CS83_East
                            if (json_out) {
                              json_geo_key_entry["projection_geo_key"] = "Proj_Alabama_CS83_East";
                            } else {
                              fprintf(file_out, "ProjectionGeoKey: Proj_Alabama_CS83_East\012");
                            }
                            break;
                          case 10132:  // Proj_Alabama_CS83_West
                            if (json_out) {
                              json_geo_key_entry["projection_geo_key"] = "Proj_Alabama_CS83_West";
                            } else {
                              fprintf(file_out, "ProjectionGeoKey: Proj_Alabama_CS83_West\012");
                            }
                            break;
                          case 10201:  // Proj_Arizona_Coordinate_System_east
                            if (json_out) {
                              json_geo_key_entry["projection_geo_key"] = "Proj_Arizona_Coordinate_System_east";
                            } else {
                              fprintf(file_out, "ProjectionGeoKey: Proj_Arizona_Coordinate_System_east\012");
                            }
                            break;
                          case 10202:  // Proj_Arizona_Coordinate_System_Central
                            if (json_out) {
                              json_geo_key_entry["projection_geo_key"] = "Proj_Arizona_Coordinate_System_Central";
                            } else {
                              fprintf(file_out, "ProjectionGeoKey: Proj_Arizona_Coordinate_System_Central\012");
                            }
                            break;
                          case 10203:  // Proj_Arizona_Coordinate_System_west
                            if (json_out) {
                              json_geo_key_entry["projection_geo_key"] = "Proj_Arizona_Coordinate_System_west";
                            } else {
                              fprintf(file_out, "ProjectionGeoKey: Proj_Arizona_Coordinate_System_west\012");
                            }
                            break;
                          case 10231:  // Proj_Arizona_CS83_east
                            if (json_out) {
                              json_geo_key_entry["projection_geo_key"] = "Proj_Arizona_CS83_east";
                            } else {
                              fprintf(file_out, "ProjectionGeoKey: Proj_Arizona_CS83_east\012");
                            }
                            break;
                          case 10232:  // Proj_Arizona_CS83_Central
                            if (json_out) {
                              json_geo_key_entry["projection_geo_key"] = "Proj_Arizona_CS83_Central";
                            } else {
                              fprintf(file_out, "ProjectionGeoKey: Proj_Arizona_CS83_Central\012");
                            }
                            break;
                          case 10233:  // Proj_Arizona_CS83_west
                            if (json_out) {
                              json_geo_key_entry["projection_geo_key"] = "Proj_Arizona_CS83_west";
                            } else {
                              fprintf(file_out, "ProjectionGeoKey: Proj_Arizona_CS83_west\012");
                            }
                            break;
                          case 10301:  // Proj_Arkansas_CS27_North
                            if (json_out) {
                              json_geo_key_entry["projection_geo_key"] = "Proj_Arkansas_CS27_North";
                            } else {
                              fprintf(file_out, "ProjectionGeoKey: Proj_Arkansas_CS27_North\012");
                            }
                            break;
                          case 10302:  // Proj_Arkansas_CS27_South
                            if (json_out) {
                              json_geo_key_entry["projection_geo_key"] = "Proj_Arkansas_CS27_South";
                            } else {
                              fprintf(file_out, "ProjectionGeoKey: Proj_Arkansas_CS27_South\012");
                            }
                            break;
                          case 10331:  // Proj_Arkansas_CS83_North
                            if (json_out) {
                              json_geo_key_entry["projection_geo_key"] = "Proj_Arkansas_CS83_North";
                            } else {
                              fprintf(file_out, "ProjectionGeoKey: Proj_Arkansas_CS83_North\012");
                            }
                            break;
                          case 10332:  // Proj_Arkansas_CS83_South
                            if (json_out) {
                              json_geo_key_entry["projection_geo_key"] = "Proj_Arkansas_CS83_South";
                            } else {
                              fprintf(file_out, "ProjectionGeoKey: Proj_Arkansas_CS83_South\012");
                            }
                            break;
                          case 10401:  // Proj_California_CS27_I
                            if (json_out) {
                              json_geo_key_entry["projection_geo_key"] = "Proj_California_CS27_I";
                            } else {
                              fprintf(file_out, "ProjectionGeoKey: Proj_California_CS27_I\012");
                            }
                            break;
                          case 10402:  // Proj_California_CS27_II
                            if (json_out) {
                              json_geo_key_entry["projection_geo_key"] = "Proj_California_CS27_II";
                            } else {
                              fprintf(file_out, "ProjectionGeoKey: Proj_California_CS27_II\012");
                            }
                            break;
                          case 10403:  // Proj_California_CS27_III
                            if (json_out) {
                              json_geo_key_entry["projection_geo_key"] = "Proj_California_CS27_III";
                            } else {
                              fprintf(file_out, "ProjectionGeoKey: Proj_California_CS27_III\012");
                            }
                            break;
                          case 10404:  // Proj_California_CS27_IV
                            if (json_out) {
                              json_geo_key_entry["projection_geo_key"] = "Proj_California_CS27_IV";
                            } else {
                              fprintf(file_out, "ProjectionGeoKey: Proj_California_CS27_IV\012");
                            }
                            break;
                          case 10405:  // Proj_California_CS27_V
                            if (json_out) {
                              json_geo_key_entry["projection_geo_key"] = "Proj_California_CS27_V";
                            } else {
                              fprintf(file_out, "ProjectionGeoKey: Proj_California_CS27_V\012");
                            }
                            break;
                          case 10406:  // Proj_California_CS27_VI
                            if (json_out) {
                              json_geo_key_entry["projection_geo_key"] = "Proj_California_CS27_VI";
                            } else {
                              fprintf(file_out, "ProjectionGeoKey: Proj_California_CS27_VI\012");
                            }
                            break;
                          case 10407:  // Proj_California_CS27_VII
                            if (json_out) {
                              json_geo_key_entry["projection_geo_key"] = "Proj_California_CS27_VII";
                            } else {
                              fprintf(file_out, "ProjectionGeoKey: Proj_California_CS27_VII\012");
                            }
                            break;
                          case 10431:  // Proj_California_CS83_1
                            if (json_out) {
                              json_geo_key_entry["projection_geo_key"] = "Proj_California_CS83_1";
                            } else {
                              fprintf(file_out, "ProjectionGeoKey: Proj_California_CS83_1\012");
                            }
                            break;
                          case 10432:  // Proj_California_CS83_2
                            if (json_out) {
                              json_geo_key_entry["projection_geo_key"] = "Proj_California_CS83_2";
                            } else {
                              fprintf(file_out, "ProjectionGeoKey: Proj_California_CS83_2\012");
                            }
                            break;
                          case 10433:  // Proj_California_CS83_3
                            if (json_out) {
                              json_geo_key_entry["projection_geo_key"] = "Proj_California_CS83_3";
                            } else {
                              fprintf(file_out, "ProjectionGeoKey: Proj_California_CS83_3\012");
                            }
                            break;
                          case 10434:  // Proj_California_CS83_4
                            if (json_out) {
                              json_geo_key_entry["projection_geo_key"] = "Proj_California_CS83_4";
                            } else {
                              fprintf(file_out, "ProjectionGeoKey: Proj_California_CS83_4\012");
                            }
                            break;
                          case 10435:  // Proj_California_CS83_5
                            if (json_out) {
                              json_geo_key_entry["projection_geo_key"] = "Proj_California_CS83_5";
                            } else {
                              fprintf(file_out, "ProjectionGeoKey: Proj_California_CS83_5\012");
                            }
                            break;
                          case 10436:  // Proj_California_CS83_6
                            if (json_out) {
                              json_geo_key_entry["projection_geo_key"] = "Proj_California_CS83_6";
                            } else {
                              fprintf(file_out, "ProjectionGeoKey: Proj_California_CS83_6\012");
                            }
                            break;
                          case 10501:  // Proj_Colorado_CS27_North
                            if (json_out) {
                              json_geo_key_entry["projection_geo_key"] = "Proj_Colorado_CS27_North";
                            } else {
                              fprintf(file_out, "ProjectionGeoKey: Proj_Colorado_CS27_North\012");
                            }
                            break;
                          case 10502:  // Proj_Colorado_CS27_Central
                            if (json_out) {
                              json_geo_key_entry["projection_geo_key"] = "Proj_Colorado_CS27_Central";
                            } else {
                              fprintf(file_out, "ProjectionGeoKey: Proj_Colorado_CS27_Central\012");
                            }
                            break;
                          case 10503:  // Proj_Colorado_CS27_South
                            if (json_out) {
                              json_geo_key_entry["projection_geo_key"] = "Proj_Colorado_CS27_South";
                            } else {
                              fprintf(file_out, "ProjectionGeoKey: Proj_Colorado_CS27_South\012");
                            }
                            break;
                          case 10531:  // Proj_Colorado_CS83_North
                            if (json_out) {
                              json_geo_key_entry["projection_geo_key"] = "Proj_Colorado_CS83_North";
                            } else {
                              fprintf(file_out, "ProjectionGeoKey: Proj_Colorado_CS83_North\012");
                            }
                            break;
                          case 10532:  // Proj_Colorado_CS83_Central
                            if (json_out) {
                              json_geo_key_entry["projection_geo_key"] = "Proj_Colorado_CS83_Central";
                            } else {
                              fprintf(file_out, "ProjectionGeoKey: Proj_Colorado_CS83_Central\012");
                            }
                            break;
                          case 10533:  // Proj_Colorado_CS83_South
                            if (json_out) {
                              json_geo_key_entry["projection_geo_key"] = "Proj_Colorado_CS83_South";
                            } else {
                              fprintf(file_out, "ProjectionGeoKey: Proj_Colorado_CS83_South\012");
                            }
                            break;
                          case 10600:  // Proj_Connecticut_CS27
                            if (json_out) {
                              json_geo_key_entry["projection_geo_key"] = "Proj_Connecticut_CS27";
                            } else {
                              fprintf(file_out, "ProjectionGeoKey: Proj_Connecticut_CS27\012");
                            }
                            break;
                          case 10630:  // Proj_Connecticut_CS83
                            if (json_out) {
                              json_geo_key_entry["projection_geo_key"] = "Proj_Connecticut_CS83";
                            } else {
                              fprintf(file_out, "ProjectionGeoKey: Proj_Connecticut_CS83\012");
                            }
                            break;
                          case 10700:  // Proj_Delaware_CS27
                            if (json_out) {
                              json_geo_key_entry["projection_geo_key"] = "Proj_Delaware_CS27";
                            } else {
                              fprintf(file_out, "ProjectionGeoKey: Proj_Delaware_CS27\012");
                            }
                            break;
                          case 10730:  // Proj_Delaware_CS83
                            if (json_out) {
                              json_geo_key_entry["projection_geo_key"] = "Proj_Delaware_CS83";
                            } else {
                              fprintf(file_out, "ProjectionGeoKey: Proj_Delaware_CS83\012");
                            }
                            break;
                          case 10901:  // Proj_Florida_CS27_East
                            if (json_out) {
                              json_geo_key_entry["projection_geo_key"] = "Proj_Florida_CS27_East";
                            } else {
                              fprintf(file_out, "ProjectionGeoKey: Proj_Florida_CS27_East\012");
                            }
                            break;
                          case 10902:  // Proj_Florida_CS27_West
                            if (json_out) {
                              json_geo_key_entry["projection_geo_key"] = "Proj_Florida_CS27_West";
                            } else {
                              fprintf(file_out, "ProjectionGeoKey: Proj_Florida_CS27_West\012");
                            }
                            break;
                          case 10903:  // Proj_Florida_CS27_North
                            if (json_out) {
                              json_geo_key_entry["projection_geo_key"] = "Proj_Florida_CS27_North";
                            } else {
                              fprintf(file_out, "ProjectionGeoKey: Proj_Florida_CS27_North\012");
                            }
                            break;
                          case 10931:  // Proj_Florida_CS83_East
                            if (json_out) {
                              json_geo_key_entry["projection_geo_key"] = "Proj_Florida_CS83_East";
                            } else {
                              fprintf(file_out, "ProjectionGeoKey: Proj_Florida_CS83_East\012");
                            }
                            break;
                          case 10932:  // Proj_Florida_CS83_West
                            if (json_out) {
                              json_geo_key_entry["projection_geo_key"] = "Proj_Florida_CS83_West";
                            } else {
                              fprintf(file_out, "ProjectionGeoKey: Proj_Florida_CS83_West\012");
                            }
                            break;
                          case 10933:  // Proj_Florida_CS83_North
                            if (json_out) {
                              json_geo_key_entry["projection_geo_key"] = "Proj_Florida_CS83_North";
                            } else {
                              fprintf(file_out, "ProjectionGeoKey: Proj_Florida_CS83_North\012");
                            }
                            break;
                          case 11001:  // Proj_Georgia_CS27_East
                            if (json_out) {
                              json_geo_key_entry["projection_geo_key"] = "Proj_Georgia_CS27_East";
                            } else {
                              fprintf(file_out, "ProjectionGeoKey: Proj_Georgia_CS27_East\012");
                            }
                            break;
                          case 11002:  // Proj_Georgia_CS27_West
                            if (json_out) {
                              json_geo_key_entry["projection_geo_key"] = "Proj_Georgia_CS27_West";
                            } else {
                              fprintf(file_out, "ProjectionGeoKey: Proj_Georgia_CS27_West\012");
                            }
                            break;
                          case 11031:  // Proj_Georgia_CS83_East
                            if (json_out) {
                              json_geo_key_entry["projection_geo_key"] = "Proj_Georgia_CS83_East";
                            } else {
                              fprintf(file_out, "ProjectionGeoKey: Proj_Georgia_CS83_East\012");
                            }
                            break;
                          case 11032:  // Proj_Georgia_CS83_West
                            if (json_out) {
                              json_geo_key_entry["projection_geo_key"] = "Proj_Georgia_CS83_East";
                            } else {
                              fprintf(file_out, "ProjectionGeoKey: Proj_Georgia_CS83_East\012");
                            }
                            break;
                          case 11101:  // Proj_Idaho_CS27_East
                            if (json_out) {
                              json_geo_key_entry["projection_geo_key"] = "Proj_Idaho_CS27_East";
                            } else {
                              fprintf(file_out, "ProjectionGeoKey: Proj_Idaho_CS27_East\012");
                            }
                            break;
                          case 11102:  // Proj_Idaho_CS27_Central
                            if (json_out) {
                              json_geo_key_entry["projection_geo_key"] = "Proj_Idaho_CS27_Central";
                            } else {
                              fprintf(file_out, "ProjectionGeoKey: Proj_Idaho_CS27_Central\012");
                            }
                            break;
                          case 11103:  // Proj_Idaho_CS27_West
                            if (json_out) {
                              json_geo_key_entry["projection_geo_key"] = "Proj_Idaho_CS27_West";
                            } else {
                              fprintf(file_out, "ProjectionGeoKey: Proj_Idaho_CS27_West\012");
                            }
                            break;
                          case 11131:  // Proj_Idaho_CS83_East
                            if (json_out) {
                              json_geo_key_entry["projection_geo_key"] = "Proj_Idaho_CS83_East";
                            } else {
                              fprintf(file_out, "ProjectionGeoKey: Proj_Idaho_CS83_East\012");
                            }
                            break;
                          case 11132:  // Proj_Idaho_CS83_Central
                            if (json_out) {
                              json_geo_key_entry["projection_geo_key"] = "Proj_Idaho_CS83_Central";
                            } else {
                              fprintf(file_out, "ProjectionGeoKey: Proj_Idaho_CS83_Central\012");
                            }
                            break;
                          case 11133:  // Proj_Idaho_CS83_West
                            if (json_out) {
                              json_geo_key_entry["projection_geo_key"] = "Proj_Idaho_CS83_West";
                            } else {
                              fprintf(file_out, "ProjectionGeoKey: Proj_Idaho_CS83_West\012");
                            }
                            break;
                          case 11201:  // Proj_Illinois_CS27_East
                            if (json_out) {
                              json_geo_key_entry["projection_geo_key"] = "Proj_Illinois_CS27_East";
                            } else {
                              fprintf(file_out, "ProjectionGeoKey: Proj_Illinois_CS27_East\012");
                            }
                            break;
                          case 11202:  // Proj_Illinois_CS27_West
                            if (json_out) {
                              json_geo_key_entry["projection_geo_key"] = "Proj_Illinois_CS27_West";
                            } else {
                              fprintf(file_out, "ProjectionGeoKey: Proj_Illinois_CS27_West\012");
                            }
                            break;
                          case 11231:  // Proj_Illinois_CS83_East
                            if (json_out) {
                              json_geo_key_entry["projection_geo_key"] = "Proj_Illinois_CS83_East";
                            } else {
                              fprintf(file_out, "ProjectionGeoKey: Proj_Illinois_CS83_East\012");
                            }
                            break;
                          case 11232:  // Proj_Illinois_CS83_West
                            if (json_out) {
                              json_geo_key_entry["projection_geo_key"] = "Proj_Illinois_CS83_West";
                            } else {
                              fprintf(file_out, "ProjectionGeoKey: Proj_Illinois_CS83_West\012");
                            }
                            break;
                          case 11301:  // Proj_Indiana_CS27_East
                            if (json_out) {
                              json_geo_key_entry["projection_geo_key"] = "Proj_Indiana_CS27_East";
                            } else {
                              fprintf(file_out, "ProjectionGeoKey: Proj_Indiana_CS27_East\012");
                            }
                            break;
                          case 11302:  // Proj_Indiana_CS27_West
                            if (json_out) {
                              json_geo_key_entry["projection_geo_key"] = "Proj_Indiana_CS27_West";
                            } else {
                              fprintf(file_out, "ProjectionGeoKey: Proj_Indiana_CS27_West\012");
                            }
                            break;
                          case 11331:  // Proj_Indiana_CS83_East
                            if (json_out) {
                              json_geo_key_entry["projection_geo_key"] = "Proj_Indiana_CS83_East";
                            } else {
                              fprintf(file_out, "ProjectionGeoKey: Proj_Indiana_CS83_East\012");
                            }
                            break;
                          case 11332:  // Proj_Indiana_CS83_West
                            if (json_out) {
                              json_geo_key_entry["projection_geo_key"] = "Proj_Indiana_CS83_West";
                            } else {
                              fprintf(file_out, "ProjectionGeoKey: Proj_Indiana_CS83_West\012");
                            }
                            break;
                          case 11401:  // Proj_Iowa_CS27_North
                            if (json_out) {
                              json_geo_key_entry["projection_geo_key"] = "Proj_Iowa_CS27_North";
                            } else {
                              fprintf(file_out, "ProjectionGeoKey: Proj_Iowa_CS27_North\012");
                            }
                            break;
                          case 11402:  // Proj_Iowa_CS27_South
                            if (json_out) {
                              json_geo_key_entry["projection_geo_key"] = "Proj_Iowa_CS27_South";
                            } else {
                              fprintf(file_out, "ProjectionGeoKey: Proj_Iowa_CS27_South\012");
                            }
                            break;
                          case 11431:  // Proj_Iowa_CS83_North
                            if (json_out) {
                              json_geo_key_entry["projection_geo_key"] = "Proj_Iowa_CS83_North";
                            } else {
                              fprintf(file_out, "ProjectionGeoKey: Proj_Iowa_CS83_North\012");
                            }
                            break;
                          case 11432:  // Proj_Iowa_CS83_South
                            if (json_out) {
                              json_geo_key_entry["projection_geo_key"] = "Proj_Iowa_CS83_South";
                            } else {
                              fprintf(file_out, "ProjectionGeoKey: Proj_Iowa_CS83_South\012");
                            }
                            break;
                          case 11501:  // Proj_Kansas_CS27_North
                            if (json_out) {
                              json_geo_key_entry["projection_geo_key"] = "Proj_Kansas_CS27_North";
                            } else {
                              fprintf(file_out, "ProjectionGeoKey: Proj_Kansas_CS27_North\012");
                            }
                            break;
                          case 11502:  // Proj_Kansas_CS27_South
                            if (json_out) {
                              json_geo_key_entry["projection_geo_key"] = "Proj_Kansas_CS27_South";
                            } else {
                              fprintf(file_out, "ProjectionGeoKey: Proj_Kansas_CS27_South\012");
                            }
                            break;
                          case 11531:  // Proj_Kansas_CS83_North
                            if (json_out) {
                              json_geo_key_entry["projection_geo_key"] = "Proj_Kansas_CS83_North";
                            } else {
                              fprintf(file_out, "ProjectionGeoKey: Proj_Kansas_CS83_North\012");
                            }
                            break;
                          case 11532:  // Proj_Kansas_CS83_South
                            if (json_out) {
                              json_geo_key_entry["projection_geo_key"] = "Proj_Kansas_CS83_South";
                            } else {
                              fprintf(file_out, "ProjectionGeoKey: Proj_Kansas_CS83_South\012");
                            }
                            break;
                          case 11601:  // Proj_Kentucky_CS27_North
                            if (json_out) {
                              json_geo_key_entry["projection_geo_key"] = "Proj_Kentucky_CS27_North";
                            } else {
                              fprintf(file_out, "ProjectionGeoKey: Proj_Kentucky_CS27_North\012");
                            }
                            break;
                          case 11602:  // Proj_Kentucky_CS27_South
                            if (json_out) {
                              json_geo_key_entry["projection_geo_key"] = "Proj_Kentucky_CS27_South";
                            } else {
                              fprintf(file_out, "ProjectionGeoKey: Proj_Kentucky_CS27_South\012");
                            }
                            break;
                          case 11631:  // Proj_Kentucky_CS83_North
                            if (json_out) {
                              json_geo_key_entry["projection_geo_key"] = "Proj_Kentucky_CS83_North";
                            } else {
                              fprintf(file_out, "ProjectionGeoKey: Proj_Kentucky_CS83_North\012");
                            }
                            break;
                          case 11632:  // Proj_Kentucky_CS83_South
                            if (json_out) {
                              json_geo_key_entry["projection_geo_key"] = "Proj_Kentucky_CS83_South";
                            } else {
                              fprintf(file_out, "ProjectionGeoKey: Proj_Kentucky_CS83_South\012");
                            }
                            break;
                          case 11701:  // Proj_Louisiana_CS27_North
                            if (json_out) {
                              json_geo_key_entry["projection_geo_key"] = "Proj_Louisiana_CS27_North";
                            } else {
                              fprintf(file_out, "ProjectionGeoKey: Proj_Louisiana_CS27_North\012");
                            }
                            break;
                          case 11702:  // Proj_Louisiana_CS27_South
                            if (json_out) {
                              json_geo_key_entry["projection_geo_key"] = "Proj_Louisiana_CS27_South";
                            } else {
                              fprintf(file_out, "ProjectionGeoKey: Proj_Louisiana_CS27_South\012");
                            }
                            break;
                          case 11731:  // Proj_Louisiana_CS83_North
                            if (json_out) {
                              json_geo_key_entry["projection_geo_key"] = "Proj_Louisiana_CS83_North";
                            } else {
                              fprintf(file_out, "ProjectionGeoKey: Proj_Louisiana_CS83_North\012");
                            }
                            break;
                          case 11732:  // Proj_Louisiana_CS83_South
                            if (json_out) {
                              json_geo_key_entry["projection_geo_key"] = "Proj_Louisiana_CS83_South";
                            } else {
                              fprintf(file_out, "ProjectionGeoKey: Proj_Louisiana_CS83_South\012");
                            }
                            break;
                          case 11801:  // Proj_Maine_CS27_East
                            if (json_out) {
                              json_geo_key_entry["projection_geo_key"] = "Proj_Maine_CS27_East";
                            } else {
                              fprintf(file_out, "ProjectionGeoKey: Proj_Maine_CS27_East\012");
                            }
                            break;
                          case 11802:  // Proj_Maine_CS27_West
                            if (json_out) {
                              json_geo_key_entry["projection_geo_key"] = "Proj_Maine_CS27_West";
                            } else {
                              fprintf(file_out, "ProjectionGeoKey: Proj_Maine_CS27_West\012");
                            }
                            break;
                          case 11831:  // Proj_Maine_CS83_East
                            if (json_out) {
                              json_geo_key_entry["projection_geo_key"] = "Proj_Maine_CS83_East";
                            } else {
                              fprintf(file_out, "ProjectionGeoKey: Proj_Maine_CS83_East\012");
                            }
                            break;
                          case 11832:  // Proj_Maine_CS83_West
                            if (json_out) {
                              json_geo_key_entry["projection_geo_key"] = "Proj_Maine_CS83_West";
                            } else {
                              fprintf(file_out, "ProjectionGeoKey: Proj_Maine_CS83_West\012");
                            }
                            break;
                          case 11900:  // Proj_Maryland_CS27
                            if (json_out) {
                              json_geo_key_entry["projection_geo_key"] = "Proj_Maryland_CS27";
                            } else {
                              fprintf(file_out, "ProjectionGeoKey: Proj_Maryland_CS27\012");
                            }
                            break;
                          case 11930:  // Proj_Maryland_CS83
                            if (json_out) {
                              json_geo_key_entry["projection_geo_key"] = "Proj_Maryland_CS83";
                            } else {
                              fprintf(file_out, "ProjectionGeoKey: Proj_Maryland_CS83\012");
                            }
                            break;
                          case 12001:  // Proj_Massachusetts_CS27_Mainland
                            if (json_out) {
                              json_geo_key_entry["projection_geo_key"] = "Proj_Massachusetts_CS27_Mainland";
                            } else {
                              fprintf(file_out, "ProjectionGeoKey: Proj_Massachusetts_CS27_Mainland\012");
                            }
                            break;
                          case 12002:  // Proj_Massachusetts_CS27_Island
                            if (json_out) {
                              json_geo_key_entry["projection_geo_key"] = "Proj_Massachusetts_CS27_Island";
                            } else {
                              fprintf(file_out, "ProjectionGeoKey: Proj_Massachusetts_CS27_Island\012");
                            }
                            break;
                          case 12031:  // Proj_Massachusetts_CS83_Mainland
                            if (json_out) {
                              json_geo_key_entry["projection_geo_key"] = "Proj_Massachusetts_CS83_Mainland";
                            } else {
                              fprintf(file_out, "ProjectionGeoKey: Proj_Massachusetts_CS83_Mainland\012");
                            }
                            break;
                          case 12032:  // Proj_Massachusetts_CS83_Island
                            if (json_out) {
                              json_geo_key_entry["projection_geo_key"] = "Proj_Massachusetts_CS83_Island";
                            } else {
                              fprintf(file_out, "ProjectionGeoKey: Proj_Massachusetts_CS83_Island\012");
                            }
                            break;
                          case 12101:  // Proj_Michigan_State_Plane_East
                            if (json_out) {
                              json_geo_key_entry["projection_geo_key"] = "Proj_Michigan_State_Plane_East";
                            } else {
                              fprintf(file_out, "ProjectionGeoKey: Proj_Michigan_State_Plane_East\012");
                            }
                            break;
                          case 12102:  // Proj_Michigan_State_Plane_Old_Central
                            if (json_out) {
                              json_geo_key_entry["projection_geo_key"] = "Proj_Michigan_State_Plane_Old_Central";
                            } else {
                              fprintf(file_out, "ProjectionGeoKey: Proj_Michigan_State_Plane_Old_Central\012");
                            }
                            break;
                          case 12103:  // Proj_Michigan_State_Plane_West
                            if (json_out) {
                              json_geo_key_entry["projection_geo_key"] = "Proj_Michigan_State_Plane_West";
                            } else {
                              fprintf(file_out, "ProjectionGeoKey: Proj_Michigan_State_Plane_West\012");
                            }
                            break;
                          case 12111:  // Proj_Michigan_CS27_North
                            if (json_out) {
                              json_geo_key_entry["projection_geo_key"] = "Proj_Michigan_CS27_North";
                            } else {
                              fprintf(file_out, "ProjectionGeoKey: Proj_Michigan_CS27_North\012");
                            }
                            break;
                          case 12112:  // Proj_Michigan_CS27_Central
                            if (json_out) {
                              json_geo_key_entry["projection_geo_key"] = "Proj_Michigan_CS27_Central";
                            } else {
                              fprintf(file_out, "ProjectionGeoKey: Proj_Michigan_CS27_Central\012");
                            }
                            break;
                          case 12113:  // Proj_Michigan_CS27_South
                            if (json_out) {
                              json_geo_key_entry["projection_geo_key"] = "Proj_Michigan_CS27_South";
                            } else {
                              fprintf(file_out, "ProjectionGeoKey: Proj_Michigan_CS27_South\012");
                            }
                            break;
                          case 12141:  // Proj_Michigan_CS83_North
                            if (json_out) {
                              json_geo_key_entry["projection_geo_key"] = "Proj_Michigan_CS83_North";
                            } else {
                              fprintf(file_out, "ProjectionGeoKey: Proj_Michigan_CS83_North\012");
                            }
                            break;
                          case 12142:  // Proj_Michigan_CS83_Central
                            if (json_out) {
                              json_geo_key_entry["projection_geo_key"] = "Proj_Michigan_CS83_Central";
                            } else {
                              fprintf(file_out, "ProjectionGeoKey: Proj_Michigan_CS83_Central\012");
                            }
                            break;
                          case 12143:  // Proj_Michigan_CS83_South
                            if (json_out) {
                              json_geo_key_entry["projection_geo_key"] = "Proj_Michigan_CS83_South";
                            } else {
                              fprintf(file_out, "ProjectionGeoKey: Proj_Michigan_CS83_South\012");
                            }
                            break;
                          case 12201:  // Proj_Minnesota_CS27_North
                            if (json_out) {
                              json_geo_key_entry["projection_geo_key"] = "Proj_Minnesota_CS27_North";
                            } else {
                              fprintf(file_out, "ProjectionGeoKey: Proj_Minnesota_CS27_North\012");
                            }
                            break;
                          case 12202:  // Proj_Minnesota_CS27_Central
                            if (json_out) {
                              json_geo_key_entry["projection_geo_key"] = "Proj_Minnesota_CS27_Central";
                            } else {
                              fprintf(file_out, "ProjectionGeoKey: Proj_Minnesota_CS27_Central\012");
                            }
                            break;
                          case 12203:  // Proj_Minnesota_CS27_South
                            if (json_out) {
                              json_geo_key_entry["projection_geo_key"] = "Proj_Minnesota_CS27_South";
                            } else {
                              fprintf(file_out, "ProjectionGeoKey: Proj_Minnesota_CS27_South\012");
                            }
                            break;
                          case 12231:  // Proj_Minnesota_CS83_North
                            if (json_out) {
                              json_geo_key_entry["projection_geo_key"] = "Proj_Minnesota_CS83_North";
                            } else {
                              fprintf(file_out, "ProjectionGeoKey: Proj_Minnesota_CS83_North\012");
                            }
                            break;
                          case 12232:  // Proj_Minnesota_CS83_Central
                            if (json_out) {
                              json_geo_key_entry["projection_geo_key"] = "Proj_Minnesota_CS83_Central";
                            } else {
                              fprintf(file_out, "ProjectionGeoKey: Proj_Minnesota_CS83_Central\012");
                            }
                            break;
                          case 12233:  // Proj_Minnesota_CS83_South
                            if (json_out) {
                              json_geo_key_entry["projection_geo_key"] = "Proj_Minnesota_CS83_South";
                            } else {
                              fprintf(file_out, "ProjectionGeoKey: Proj_Minnesota_CS83_South\012");
                            }
                            break;
                          case 12301:  // Proj_Mississippi_CS27_East
                            if (json_out) {
                              json_geo_key_entry["projection_geo_key"] = "Proj_Mississippi_CS27_East";
                            } else {
                              fprintf(file_out, "ProjectionGeoKey: Proj_Mississippi_CS27_East\012");
                            }
                            break;
                          case 12302:  // Proj_Mississippi_CS27_West
                            if (json_out) {
                              json_geo_key_entry["projection_geo_key"] = "Proj_Mississippi_CS27_West";
                            } else {
                              fprintf(file_out, "ProjectionGeoKey: Proj_Mississippi_CS27_West\012");
                            }
                            break;
                          case 12331:  // Proj_Mississippi_CS83_East
                            if (json_out) {
                              json_geo_key_entry["projection_geo_key"] = "Proj_Mississippi_CS83_East";
                            } else {
                              fprintf(file_out, "ProjectionGeoKey: Proj_Mississippi_CS83_East\012");
                            }
                            break;
                          case 12332:  // Proj_Mississippi_CS83_West
                            if (json_out) {
                              json_geo_key_entry["projection_geo_key"] = "Proj_Mississippi_CS83_West";
                            } else {
                              fprintf(file_out, "ProjectionGeoKey: Proj_Mississippi_CS83_West\012");
                            }
                            break;
                          case 12401:  // Proj_Missouri_CS27_East
                            if (json_out) {
                              json_geo_key_entry["projection_geo_key"] = "Proj_Missouri_CS27_East";
                            } else {
                              fprintf(file_out, "ProjectionGeoKey: Proj_Missouri_CS27_East\012");
                            }
                            break;
                          case 12402:  // Proj_Missouri_CS27_Central
                            if (json_out) {
                              json_geo_key_entry["projection_geo_key"] = "Proj_Missouri_CS27_Central";
                            } else {
                              fprintf(file_out, "ProjectionGeoKey: Proj_Missouri_CS27_Central\012");
                            }
                            break;
                          case 12403:  // Proj_Missouri_CS27_West
                            if (json_out) {
                              json_geo_key_entry["projection_geo_key"] = "Proj_Missouri_CS27_West";
                            } else {
                              fprintf(file_out, "ProjectionGeoKey: Proj_Missouri_CS27_West\012");
                            }
                            break;
                          case 12431:  // Proj_Missouri_CS83_East
                            if (json_out) {
                              json_geo_key_entry["projection_geo_key"] = "Proj_Missouri_CS83_East";
                            } else {
                              fprintf(file_out, "ProjectionGeoKey: Proj_Missouri_CS83_East\012");
                            }
                            break;
                          case 12432:  // Proj_Missouri_CS83_Central
                            if (json_out) {
                              json_geo_key_entry["projection_geo_key"] = "Proj_Missouri_CS83_Central";
                            } else {
                              fprintf(file_out, "ProjectionGeoKey: Proj_Missouri_CS83_Central\012");
                            }
                            break;
                          case 12433:  // Proj_Missouri_CS83_West
                            if (json_out) {
                              json_geo_key_entry["projection_geo_key"] = "Proj_Missouri_CS83_West";
                            } else {
                              fprintf(file_out, "ProjectionGeoKey: Proj_Missouri_CS83_West\012");
                            }
                            break;
                          case 12501:  // Proj_Montana_CS27_North
                            if (json_out) {
                              json_geo_key_entry["projection_geo_key"] = "Proj_Montana_CS27_North";
                            } else {
                              fprintf(file_out, "ProjectionGeoKey: Proj_Montana_CS27_North\012");
                            }
                            break;
                          case 12502:  // Proj_Montana_CS27_Central
                            if (json_out) {
                              json_geo_key_entry["projection_geo_key"] = "Proj_Montana_CS27_Central";
                            } else {
                              fprintf(file_out, "ProjectionGeoKey: Proj_Montana_CS27_Central\012");
                            }
                            break;
                          case 12503:  // Proj_Montana_CS27_South
                            if (json_out) {
                              json_geo_key_entry["projection_geo_key"] = "Proj_Montana_CS27_South";
                            } else {
                              fprintf(file_out, "ProjectionGeoKey: Proj_Montana_CS27_South\012");
                            }
                            break;
                          case 12530:  // Proj_Montana_CS83
                            if (json_out) {
                              json_geo_key_entry["projection_geo_key"] = "Proj_Montana_CS83";
                            } else {
                              fprintf(file_out, "ProjectionGeoKey: Proj_Montana_CS83\012");
                            }
                            break;
                          case 12601:  // Proj_Nebraska_CS27_North
                            if (json_out) {
                              json_geo_key_entry["projection_geo_key"] = "Proj_Nebraska_CS27_North";
                            } else {
                              fprintf(file_out, "ProjectionGeoKey: Proj_Nebraska_CS27_North\012");
                            }
                            break;
                          case 12602:  // Proj_Nebraska_CS27_South
                            if (json_out) {
                              json_geo_key_entry["projection_geo_key"] = "Proj_Nebraska_CS27_South";
                            } else {
                              fprintf(file_out, "ProjectionGeoKey: Proj_Nebraska_CS27_South\012");
                            }
                            break;
                          case 12630:  // Proj_Nebraska_CS83
                            if (json_out) {
                              json_geo_key_entry["projection_geo_key"] = "Proj_Nebraska_CS83";
                            } else {
                              fprintf(file_out, "ProjectionGeoKey: Proj_Nebraska_CS83\012");
                            }
                            break;
                          case 12701:  // Proj_Nevada_CS27_East
                            if (json_out) {
                              json_geo_key_entry["projection_geo_key"] = "Proj_Nevada_CS27_East";
                            } else {
                              fprintf(file_out, "ProjectionGeoKey: Proj_Nevada_CS27_East\012");
                            }
                            break;
                          case 12702:  // Proj_Nevada_CS27_Central
                            if (json_out) {
                              json_geo_key_entry["projection_geo_key"] = "Proj_Nevada_CS27_Central";
                            } else {
                              fprintf(file_out, "ProjectionGeoKey: Proj_Nevada_CS27_Central\012");
                            }
                            break;
                          case 12703:  // Proj_Nevada_CS27_West
                            if (json_out) {
                              json_geo_key_entry["projection_geo_key"] = "Proj_Nevada_CS27_West";
                            } else {
                              fprintf(file_out, "ProjectionGeoKey: Proj_Nevada_CS27_West\012");
                            }
                            break;
                          case 12731:  // Proj_Nevada_CS83_East
                            if (json_out) {
                              json_geo_key_entry["projection_geo_key"] = "Proj_Nevada_CS83_East";
                            } else {
                              fprintf(file_out, "ProjectionGeoKey: Proj_Nevada_CS83_East\012");
                            }
                            break;
                          case 12732:  // Proj_Nevada_CS83_Central
                            if (json_out) {
                              json_geo_key_entry["projection_geo_key"] = "Proj_Nevada_CS83_Central";
                            } else {
                              fprintf(file_out, "ProjectionGeoKey: Proj_Nevada_CS83_Central\012");
                            }
                            break;
                          case 12733:  // Proj_Nevada_CS83_West
                            if (json_out) {
                              json_geo_key_entry["projection_geo_key"] = "Proj_Nevada_CS83_West";
                            } else {
                              fprintf(file_out, "ProjectionGeoKey: Proj_Nevada_CS83_West\012");
                            }
                            break;
                          case 12800:  // Proj_New_Hampshire_CS27
                            if (json_out) {
                              json_geo_key_entry["projection_geo_key"] = "Proj_New_Hampshire_CS27";
                            } else {
                              fprintf(file_out, "ProjectionGeoKey: Proj_New_Hampshire_CS27\012");
                            }
                            break;
                          case 12830:  // Proj_New_Hampshire_CS83
                            if (json_out) {
                              json_geo_key_entry["projection_geo_key"] = "Proj_New_Hampshire_CS83";
                            } else {
                              fprintf(file_out, "ProjectionGeoKey: Proj_New_Hampshire_CS83\012");
                            }
                            break;
                          case 12900:  // Proj_New_Jersey_CS27
                            if (json_out) {
                              json_geo_key_entry["projection_geo_key"] = "Proj_New_Jersey_CS27";
                            } else {
                              fprintf(file_out, "ProjectionGeoKey: Proj_New_Jersey_CS27\012");
                            }
                            break;
                          case 12930:  // Proj_New_Jersey_CS83
                            if (json_out) {
                              json_geo_key_entry["projection_geo_key"] = "Proj_New_Jersey_CS83";
                            } else {
                              fprintf(file_out, "ProjectionGeoKey: Proj_New_Jersey_CS83\012");
                            }
                            break;
                          case 13001:  // Proj_New_Mexico_CS27_East
                            if (json_out) {
                              json_geo_key_entry["projection_geo_key"] = "Proj_New_Mexico_CS27_East";
                            } else {
                              fprintf(file_out, "ProjectionGeoKey: Proj_New_Mexico_CS27_East\012");
                            }
                            break;
                          case 13002:  // Proj_New_Mexico_CS27_Central
                            if (json_out) {
                              json_geo_key_entry["projection_geo_key"] = "Proj_New_Mexico_CS27_Central";
                            } else {
                              fprintf(file_out, "ProjectionGeoKey: Proj_New_Mexico_CS27_Central\012");
                            }
                            break;
                          case 13003:  // Proj_New_Mexico_CS27_West
                            if (json_out) {
                              json_geo_key_entry["projection_geo_key"] = "Proj_New_Mexico_CS27_West";
                            } else {
                              fprintf(file_out, "ProjectionGeoKey: Proj_New_Mexico_CS27_West\012");
                            }
                            break;
                          case 13031:  // Proj_New_Mexico_CS83_East
                            if (json_out) {
                              json_geo_key_entry["projection_geo_key"] = "Proj_New_Mexico_CS83_East";
                            } else {
                              fprintf(file_out, "ProjectionGeoKey: Proj_New_Mexico_CS83_East\012");
                            }
                            break;
                          case 13032:  // Proj_New_Mexico_CS83_Central
                            if (json_out) {
                              json_geo_key_entry["projection_geo_key"] = "Proj_New_Mexico_CS83_Central";
                            } else {
                              fprintf(file_out, "ProjectionGeoKey: Proj_New_Mexico_CS83_Central\012");
                            }
                            break;
                          case 13033:  // Proj_New_Mexico_CS83_West
                            if (json_out) {
                              json_geo_key_entry["projection_geo_key"] = "Proj_New_Mexico_CS83_West";
                            } else {
                              fprintf(file_out, "ProjectionGeoKey: Proj_New_Mexico_CS83_West\012");
                            }
                            break;
                          case 13101:  // Proj_New_York_CS27_East
                            if (json_out) {
                              json_geo_key_entry["projection_geo_key"] = "Proj_New_York_CS27_East";
                            } else {
                              fprintf(file_out, "ProjectionGeoKey: Proj_New_York_CS27_East\012");
                            }
                            break;
                          case 13102:  // Proj_New_York_CS27_Central
                            if (json_out) {
                              json_geo_key_entry["projection_geo_key"] = "Proj_New_York_CS27_Central";
                            } else {
                              fprintf(file_out, "ProjectionGeoKey: Proj_New_York_CS27_Central\012");
                            }
                            break;
                          case 13103:  // Proj_New_York_CS27_West
                            if (json_out) {
                              json_geo_key_entry["projection_geo_key"] = "Proj_New_York_CS27_West";
                            } else {
                              fprintf(file_out, "ProjectionGeoKey: Proj_New_York_CS27_West\012");
                            }
                            break;
                          case 13104:  // Proj_New_York_CS27_Long_Island
                            if (json_out) {
                              json_geo_key_entry["projection_geo_key"] = "Proj_New_York_CS27_Long_Island";
                            } else {
                              fprintf(file_out, "ProjectionGeoKey: Proj_New_York_CS27_Long_Island\012");
                            }
                            break;
                          case 13131:  // Proj_New_York_CS83_East
                            if (json_out) {
                              json_geo_key_entry["projection_geo_key"] = "Proj_New_York_CS83_East";
                            } else {
                              fprintf(file_out, "ProjectionGeoKey: Proj_New_York_CS83_East\012");
                            }
                            break;
                          case 13132:  // Proj_New_York_CS83_Central
                            if (json_out) {
                              json_geo_key_entry["projection_geo_key"] = "Proj_New_York_CS83_Central";
                            } else {
                              fprintf(file_out, "ProjectionGeoKey: Proj_New_York_CS83_Central\012");
                            }
                            break;
                          case 13133:  // Proj_New_York_CS83_West
                            if (json_out) {
                              json_geo_key_entry["projection_geo_key"] = "Proj_New_York_CS83_West";
                            } else {
                              fprintf(file_out, "ProjectionGeoKey: Proj_New_York_CS83_West\012");
                            }
                            break;
                          case 13134:  // Proj_New_York_CS83_Long_Island
                            if (json_out) {
                              json_geo_key_entry["projection_geo_key"] = "Proj_New_York_CS83_Long_Island";
                            } else {
                              fprintf(file_out, "ProjectionGeoKey: Proj_New_York_CS83_Long_Island\012");
                            }
                            break;
                          case 13200:  // Proj_North_Carolina_CS27
                            if (json_out) {
                              json_geo_key_entry["projection_geo_key"] = "Proj_North_Carolina_CS27";
                            } else {
                              fprintf(file_out, "ProjectionGeoKey: Proj_North_Carolina_CS27\012");
                            }
                            break;
                          case 13230:  // Proj_North_Carolina_CS83
                            if (json_out) {
                              json_geo_key_entry["projection_geo_key"] = "Proj_North_Carolina_CS83";
                            } else {
                              fprintf(file_out, "ProjectionGeoKey: Proj_North_Carolina_CS83\012");
                            }
                            break;
                          case 13301:  // Proj_North_Dakota_CS27_North
                            if (json_out) {
                              json_geo_key_entry["projection_geo_key"] = "Proj_North_Dakota_CS27_North";
                            } else {
                              fprintf(file_out, "ProjectionGeoKey: Proj_North_Dakota_CS27_North\012");
                            }
                            break;
                          case 13302:  // Proj_North_Dakota_CS27_South
                            if (json_out) {
                              json_geo_key_entry["projection_geo_key"] = "Proj_North_Dakota_CS27_South";
                            } else {
                              fprintf(file_out, "ProjectionGeoKey: Proj_North_Dakota_CS27_South\012");
                            }
                            break;
                          case 13331:  // Proj_North_Dakota_CS83_North
                            if (json_out) {
                              json_geo_key_entry["projection_geo_key"] = "Proj_North_Dakota_CS83_North";
                            } else {
                              fprintf(file_out, "ProjectionGeoKey: Proj_North_Dakota_CS83_North\012");
                            }
                            break;
                          case 13332:  // Proj_North_Dakota_CS83_South
                            if (json_out) {
                              json_geo_key_entry["projection_geo_key"] = "Proj_North_Dakota_CS83_South";
                            } else {
                              fprintf(file_out, "ProjectionGeoKey: Proj_North_Dakota_CS83_South\012");
                            }
                            break;
                          case 13401:  // Proj_Ohio_CS27_North
                            if (json_out) {
                              json_geo_key_entry["projection_geo_key"] = "Proj_Ohio_CS27_North";
                            } else {
                              fprintf(file_out, "ProjectionGeoKey: Proj_Ohio_CS27_North\012");
                            }
                            break;
                          case 13402:  // Proj_Ohio_CS27_South
                            if (json_out) {
                              json_geo_key_entry["projection_geo_key"] = "Proj_Ohio_CS27_South";
                            } else {
                              fprintf(file_out, "ProjectionGeoKey: Proj_Ohio_CS27_South\012");
                            }
                            break;
                          case 13431:  // Proj_Ohio_CS83_North
                            if (json_out) {
                              json_geo_key_entry["projection_geo_key"] = "Proj_Ohio_CS83_North";
                            } else {
                              fprintf(file_out, "ProjectionGeoKey: Proj_Ohio_CS83_North\012");
                            }
                            break;
                          case 13432:  // Proj_Ohio_CS83_South
                            if (json_out) {
                              json_geo_key_entry["projection_geo_key"] = "Proj_Ohio_CS83_South";
                            } else {
                              fprintf(file_out, "ProjectionGeoKey: Proj_Ohio_CS83_South\012");
                            }
                            break;
                          case 13501:  // Proj_Oklahoma_CS27_North
                            if (json_out) {
                              json_geo_key_entry["projection_geo_key"] = "Proj_Oklahoma_CS27_North";
                            } else {
                              fprintf(file_out, "ProjectionGeoKey: Proj_Oklahoma_CS27_North\012");
                            }
                            break;
                          case 13502:  // Proj_Oklahoma_CS27_South
                            if (json_out) {
                              json_geo_key_entry["projection_geo_key"] = "Proj_Oklahoma_CS27_South";
                            } else {
                              fprintf(file_out, "ProjectionGeoKey: Proj_Oklahoma_CS27_South\012");
                            }
                            break;
                          case 13531:  // Proj_Oklahoma_CS83_North
                            if (json_out) {
                              json_geo_key_entry["projection_geo_key"] = "Proj_Oklahoma_CS83_North";
                            } else {
                              fprintf(file_out, "ProjectionGeoKey: Proj_Oklahoma_CS83_North\012");
                            }
                            break;
                          case 13532:  // Proj_Oklahoma_CS83_South
                            if (json_out) {
                              json_geo_key_entry["projection_geo_key"] = "Proj_Oklahoma_CS83_South";
                            } else {
                              fprintf(file_out, "ProjectionGeoKey: Proj_Oklahoma_CS83_South\012");
                            }
                            break;
                          case 13601:  // Proj_Oregon_CS27_North
                            if (json_out) {
                              json_geo_key_entry["projection_geo_key"] = "Proj_Oregon_CS27_North";
                            } else {
                              fprintf(file_out, "ProjectionGeoKey: Proj_Oregon_CS27_North\012");
                            }
                            break;
                          case 13602:  // Proj_Oregon_CS27_South
                            if (json_out) {
                              json_geo_key_entry["projection_geo_key"] = "Proj_Oregon_CS27_South";
                            } else {
                              fprintf(file_out, "ProjectionGeoKey: Proj_Oregon_CS27_South\012");
                            }
                            break;
                          case 13631:  // Proj_Oregon_CS83_North
                            if (json_out) {
                              json_geo_key_entry["projection_geo_key"] = "Proj_Oregon_CS83_North";
                            } else {
                              fprintf(file_out, "ProjectionGeoKey: Proj_Oregon_CS83_North\012");
                            }
                            break;
                          case 13632:  // Proj_Oregon_CS83_South
                            if (json_out) {
                              json_geo_key_entry["projection_geo_key"] = "Proj_Oregon_CS83_South";
                            } else {
                              fprintf(file_out, "ProjectionGeoKey: Proj_Oregon_CS83_South\012");
                            }
                            break;
                          case 13701:  // Proj_Pennsylvania_CS27_North
                            if (json_out) {
                              json_geo_key_entry["projection_geo_key"] = "Proj_Pennsylvania_CS27_North";
                            } else {
                              fprintf(file_out, "ProjectionGeoKey: Proj_Pennsylvania_CS27_North\012");
                            }
                            break;
                          case 13702:  // Proj_Pennsylvania_CS27_South
                            if (json_out) {
                              json_geo_key_entry["projection_geo_key"] = "Proj_Pennsylvania_CS27_South";
                            } else {
                              fprintf(file_out, "ProjectionGeoKey: Proj_Pennsylvania_CS27_South\012");
                            }
                            break;
                          case 13731:  // Proj_Pennsylvania_CS83_North
                            if (json_out) {
                              json_geo_key_entry["projection_geo_key"] = "Proj_Pennsylvania_CS83_North";
                            } else {
                              fprintf(file_out, "ProjectionGeoKey: Proj_Pennsylvania_CS83_North\012");
                            }
                            break;
                          case 13732:  // Proj_Pennsylvania_CS83_South
                            if (json_out) {
                              json_geo_key_entry["projection_geo_key"] = "Proj_Pennsylvania_CS83_South";
                            } else {
                              fprintf(file_out, "ProjectionGeoKey: Proj_Pennsylvania_CS83_South\012");
                            }
                            break;
                          case 13800:  // Proj_Rhode_Island_CS27
                            if (json_out) {
                              json_geo_key_entry["projection_geo_key"] = "Proj_Rhode_Island_CS27";
                            } else {
                              fprintf(file_out, "ProjectionGeoKey: Proj_Rhode_Island_CS27\012");
                            }
                            break;
                          case 13830:  // Proj_Rhode_Island_CS83
                            if (json_out) {
                              json_geo_key_entry["projection_geo_key"] = "Proj_Rhode_Island_CS83";
                            } else {
                              fprintf(file_out, "ProjectionGeoKey: Proj_Rhode_Island_CS83\012");
                            }
                            break;
                          case 13901:  // Proj_South_Carolina_CS27_North
                            if (json_out) {
                              json_geo_key_entry["projection_geo_key"] = "Proj_South_Carolina_CS27_North";
                            } else {
                              fprintf(file_out, "ProjectionGeoKey: Proj_South_Carolina_CS27_North\012");
                            }
                            break;
                          case 13902:  // Proj_South_Carolina_CS27_South
                            if (json_out) {
                              json_geo_key_entry["projection_geo_key"] = "Proj_South_Carolina_CS27_South";
                            } else {
                              fprintf(file_out, "ProjectionGeoKey: Proj_South_Carolina_CS27_South\012");
                            }
                            break;
                          case 13930:  // Proj_South_Carolina_CS83
                            if (json_out) {
                              json_geo_key_entry["projection_geo_key"] = "Proj_South_Carolina_CS83";
                            } else {
                              fprintf(file_out, "ProjectionGeoKey: Proj_South_Carolina_CS83\012");
                            }
                            break;
                          case 14001:  // Proj_South_Dakota_CS27_North
                            if (json_out) {
                              json_geo_key_entry["projection_geo_key"] = "Proj_South_Dakota_CS27_North";
                            } else {
                              fprintf(file_out, "ProjectionGeoKey: Proj_South_Dakota_CS27_North\012");
                            }
                            break;
                          case 14002:  // Proj_South_Dakota_CS27_South
                            if (json_out) {
                              json_geo_key_entry["projection_geo_key"] = "Proj_South_Dakota_CS27_South";
                            } else {
                              fprintf(file_out, "ProjectionGeoKey: Proj_South_Dakota_CS27_South\012");
                            }
                            break;
                          case 14031:  // Proj_South_Dakota_CS83_North
                            if (json_out) {
                              json_geo_key_entry["projection_geo_key"] = "Proj_South_Dakota_CS83_North";
                            } else {
                              fprintf(file_out, "ProjectionGeoKey: Proj_South_Dakota_CS83_North\012");
                            }
                            break;
                          case 14032:  // Proj_South_Dakota_CS83_South
                            if (json_out) {
                              json_geo_key_entry["projection_geo_key"] = "Proj_South_Dakota_CS83_South";
                            } else {
                              fprintf(file_out, "ProjectionGeoKey: Proj_South_Dakota_CS83_South\012");
                            }
                            break;
                          case 14100:  // Proj_Tennessee_CS27
                            if (json_out) {
                              json_geo_key_entry["projection_geo_key"] = "Proj_Tennessee_CS27";
                            } else {
                              fprintf(file_out, "ProjectionGeoKey: Proj_Tennessee_CS27\012");
                            }
                            break;
                          case 14130:  // Proj_Tennessee_CS83
                            if (json_out) {
                              json_geo_key_entry["projection_geo_key"] = "Proj_Tennessee_CS83";
                            } else {
                              fprintf(file_out, "ProjectionGeoKey: Proj_Tennessee_CS83\012");
                            }
                            break;
                          case 14201:  // Proj_Texas_CS27_North
                            if (json_out) {
                              json_geo_key_entry["projection_geo_key"] = "Proj_Texas_CS27_North";
                            } else {
                              fprintf(file_out, "ProjectionGeoKey: Proj_Texas_CS27_North\012");
                            }
                            break;
                          case 14202:  // Proj_Texas_CS27_North_Central
                            if (json_out) {
                              json_geo_key_entry["projection_geo_key"] = "Proj_Texas_CS27_North_Central";
                            } else {
                              fprintf(file_out, "ProjectionGeoKey: Proj_Texas_CS27_North_Central\012");
                            }
                            break;
                          case 14203:  // Proj_Texas_CS27_Central
                            if (json_out) {
                              json_geo_key_entry["projection_geo_key"] = "Proj_Texas_CS27_Central";
                            } else {
                              fprintf(file_out, "ProjectionGeoKey: Proj_Texas_CS27_Central\012");
                            }
                            break;
                          case 14204:  // Proj_Texas_CS27_South_Central
                            if (json_out) {
                              json_geo_key_entry["projection_geo_key"] = "Proj_Texas_CS27_South_Central";
                            } else {
                              fprintf(file_out, "ProjectionGeoKey: Proj_Texas_CS27_South_Central\012");
                            }
                            break;
                          case 14205:  // Proj_Texas_CS27_South
                            if (json_out) {
                              json_geo_key_entry["projection_geo_key"] = "Proj_Texas_CS27_South";
                            } else {
                              fprintf(file_out, "ProjectionGeoKey: Proj_Texas_CS27_South\012");
                            }
                            break;
                          case 14231:  // Proj_Texas_CS83_North
                            if (json_out) {
                              json_geo_key_entry["projection_geo_key"] = "Proj_Texas_CS83_North";
                            } else {
                              fprintf(file_out, "ProjectionGeoKey: Proj_Texas_CS83_North\012");
                            }
                            break;
                          case 14232:  // Proj_Texas_CS83_North_Central
                            if (json_out) {
                              json_geo_key_entry["projection_geo_key"] = "Proj_Texas_CS83_North_Central";
                            } else {
                              fprintf(file_out, "ProjectionGeoKey: Proj_Texas_CS83_North_Central\012");
                            }
                            break;
                          case 14233:  // Proj_Texas_CS83_Central
                            if (json_out) {
                              json_geo_key_entry["projection_geo_key"] = "Proj_Texas_CS83_Central";
                            } else {
                              fprintf(file_out, "ProjectionGeoKey: Proj_Texas_CS83_Central\012");
                            }
                            break;
                          case 14234:  // Proj_Texas_CS83_South_Central
                            if (json_out) {
                              json_geo_key_entry["projection_geo_key"] = "Proj_Texas_CS83_South_Central";
                            } else {
                              fprintf(file_out, "ProjectionGeoKey: Proj_Texas_CS83_South_Central\012");
                            }
                            break;
                          case 14235:  // Proj_Texas_CS83_South
                            if (json_out) {
                              json_geo_key_entry["projection_geo_key"] = "Proj_Texas_CS83_South";
                            } else {
                              fprintf(file_out, "ProjectionGeoKey: Proj_Texas_CS83_South\012");
                            }
                            break;
                          case 14301:  // Proj_Utah_CS27_North
                            if (json_out) {
                              json_geo_key_entry["projection_geo_key"] = "Proj_Utah_CS27_North";
                            } else {
                              fprintf(file_out, "ProjectionGeoKey: Proj_Utah_CS27_North\012");
                            }
                            break;
                          case 14302:  // Proj_Utah_CS27_Central
                            if (json_out) {
                              json_geo_key_entry["projection_geo_key"] = "Proj_Utah_CS27_Central";
                            } else {
                              fprintf(file_out, "ProjectionGeoKey: Proj_Utah_CS27_Central\012");
                            }
                            break;
                          case 14303:  // Proj_Utah_CS27_South
                            if (json_out) {
                              json_geo_key_entry["projection_geo_key"] = "Proj_Utah_CS27_South";
                            } else {
                              fprintf(file_out, "ProjectionGeoKey: Proj_Utah_CS27_South\012");
                            }
                            break;
                          case 14331:  // Proj_Utah_CS83_North
                            if (json_out) {
                              json_geo_key_entry["projection_geo_key"] = "Proj_Utah_CS83_North";
                            } else {
                              fprintf(file_out, "ProjectionGeoKey: Proj_Utah_CS83_North\012");
                            }
                            break;
                          case 14332:  // Proj_Utah_CS83_Central
                            if (json_out) {
                              json_geo_key_entry["projection_geo_key"] = "Proj_Utah_CS83_Central";
                            } else {
                              fprintf(file_out, "ProjectionGeoKey: Proj_Utah_CS83_Central\012");
                            }
                            break;
                          case 14333:  // Proj_Utah_CS83_South
                            if (json_out) {
                              json_geo_key_entry["projection_geo_key"] = "Proj_Utah_CS83_South";
                            } else {
                              fprintf(file_out, "ProjectionGeoKey: Proj_Utah_CS83_South\012");
                            }
                            break;
                          case 14400:  // Proj_Vermont_CS27
                            if (json_out) {
                              json_geo_key_entry["projection_geo_key"] = "Proj_Vermont_CS27";
                            } else {
                              fprintf(file_out, "ProjectionGeoKey: Proj_Vermont_CS27\012");
                            }
                            break;
                          case 14430:  // Proj_Vermont_CS83
                            if (json_out) {
                              json_geo_key_entry["projection_geo_key"] = "Proj_Vermont_CS83";
                            } else {
                              fprintf(file_out, "ProjectionGeoKey: Proj_Vermont_CS83\012");
                            }
                            break;
                          case 14501:  // Proj_Virginia_CS27_North
                            if (json_out) {
                              json_geo_key_entry["projection_geo_key"] = "Proj_Virginia_CS27_North";
                            } else {
                              fprintf(file_out, "ProjectionGeoKey: Proj_Virginia_CS27_North\012");
                            }
                            break;
                          case 14502:  // Proj_Virginia_CS27_South
                            if (json_out) {
                              json_geo_key_entry["projection_geo_key"] = "Proj_Virginia_CS27_South";
                            } else {
                              fprintf(file_out, "ProjectionGeoKey: Proj_Virginia_CS27_South\012");
                            }
                            break;
                          case 14531:  // Proj_Virginia_CS83_North
                            if (json_out) {
                              json_geo_key_entry["projection_geo_key"] = "Proj_Virginia_CS83_North";
                            } else {
                              fprintf(file_out, "ProjectionGeoKey: Proj_Virginia_CS83_North\012");
                            }
                            break;
                          case 14532:  // Proj_Virginia_CS83_South
                            if (json_out) {
                              json_geo_key_entry["projection_geo_key"] = "Proj_Virginia_CS83_South";
                            } else {
                              fprintf(file_out, "ProjectionGeoKey: Proj_Virginia_CS83_South\012");
                            }
                            break;
                          case 14601:  // Proj_Washington_CS27_North
                            if (json_out) {
                              json_geo_key_entry["projection_geo_key"] = "Proj_Washington_CS27_North";
                            } else {
                              fprintf(file_out, "ProjectionGeoKey: Proj_Washington_CS27_North\012");
                            }
                            break;
                          case 14602:  // Proj_Washington_CS27_South
                            if (json_out) {
                              json_geo_key_entry["projection_geo_key"] = "Proj_Washington_CS27_South";
                            } else {
                              fprintf(file_out, "ProjectionGeoKey: Proj_Washington_CS27_South\012");
                            }
                            break;
                          case 14631:  // Proj_Washington_CS83_North
                            if (json_out) {
                              json_geo_key_entry["projection_geo_key"] = "Proj_Washington_CS83_North";
                            } else {
                              fprintf(file_out, "ProjectionGeoKey: Proj_Washington_CS83_North\012");
                            }
                            break;
                          case 14632:  // Proj_Washington_CS83_South
                            if (json_out) {
                              json_geo_key_entry["projection_geo_key"] = "Proj_Washington_CS83_South";
                            } else {
                              fprintf(file_out, "ProjectionGeoKey: Proj_Washington_CS83_South\012");
                            }
                            break;
                          case 14701:  // Proj_West_Virginia_CS27_North
                            if (json_out) {
                              json_geo_key_entry["projection_geo_key"] = "Proj_West_Virginia_CS27_North";
                            } else {
                              fprintf(file_out, "ProjectionGeoKey: Proj_West_Virginia_CS27_North\012");
                            }
                            break;
                          case 14702:  // Proj_West_Virginia_CS27_South
                            if (json_out) {
                              json_geo_key_entry["projection_geo_key"] = "Proj_West_Virginia_CS27_South";
                            } else {
                              fprintf(file_out, "ProjectionGeoKey: Proj_West_Virginia_CS27_South\012");
                            }
                            break;
                          case 14731:  // Proj_West_Virginia_CS83_North
                            if (json_out) {
                              json_geo_key_entry["projection_geo_key"] = "Proj_West_Virginia_CS83_North";
                            } else {
                              fprintf(file_out, "ProjectionGeoKey: Proj_West_Virginia_CS83_North\012");
                            }
                            break;
                          case 14732:  // Proj_West_Virginia_CS83_South
                            if (json_out) {
                              json_geo_key_entry["projection_geo_key"] = "Proj_West_Virginia_CS83_South";
                            } else {
                              fprintf(file_out, "ProjectionGeoKey: Proj_West_Virginia_CS83_South\012");
                            }
                            break;
                          case 14801:  // Proj_Wisconsin_CS27_North
                            if (json_out) {
                              json_geo_key_entry["projection_geo_key"] = "Proj_Wisconsin_CS27_North";
                            } else {
                              fprintf(file_out, "ProjectionGeoKey: Proj_Wisconsin_CS27_North\012");
                            }
                            break;
                          case 14802:  // Proj_Wisconsin_CS27_Central
                            if (json_out) {
                              json_geo_key_entry["projection_geo_key"] = "Proj_Wisconsin_CS27_Central";
                            } else {
                              fprintf(file_out, "ProjectionGeoKey: Proj_Wisconsin_CS27_Central\012");
                            }
                            break;
                          case 14803:  // Proj_Wisconsin_CS27_South
                            if (json_out) {
                              json_geo_key_entry["projection_geo_key"] = "Proj_Wisconsin_CS27_South";
                            } else {
                              fprintf(file_out, "ProjectionGeoKey: Proj_Wisconsin_CS27_South\012");
                            }
                            break;
                          case 14831:  // Proj_Wisconsin_CS83_North
                            if (json_out) {
                              json_geo_key_entry["projection_geo_key"] = "Proj_Wisconsin_CS83_North";
                            } else {
                              fprintf(file_out, "ProjectionGeoKey: Proj_Wisconsin_CS83_North\012");
                            }
                            break;
                          case 14832:  // Proj_Wisconsin_CS83_Central
                            if (json_out) {
                              json_geo_key_entry["projection_geo_key"] = "Proj_Wisconsin_CS83_Central";
                            } else {
                              fprintf(file_out, "ProjectionGeoKey: Proj_Wisconsin_CS83_Central\012");
                            }
                            break;
                          case 14833:  // Proj_Wisconsin_CS83_South
                            if (json_out) {
                              json_geo_key_entry["projection_geo_key"] = "Proj_Wisconsin_CS83_South";
                            } else {
                              fprintf(file_out, "ProjectionGeoKey: Proj_Wisconsin_CS83_South\012");
                            }
                            break;
                          case 14901:  // Proj_Wyoming_CS27_East
                            if (json_out) {
                              json_geo_key_entry["projection_geo_key"] = "Proj_Wyoming_CS27_East";
                            } else {
                              fprintf(file_out, "ProjectionGeoKey: Proj_Wyoming_CS27_East\012");
                            }
                            break;
                          case 14902:  // Proj_Wyoming_CS27_East_Central
                            if (json_out) {
                              json_geo_key_entry["projection_geo_key"] = "Proj_Wyoming_CS27_East_Central";
                            } else {
                              fprintf(file_out, "ProjectionGeoKey: Proj_Wyoming_CS27_East_Central\012");
                            }
                            break;
                          case 14903:  // Proj_Wyoming_CS27_West_Central
                            if (json_out) {
                              json_geo_key_entry["projection_geo_key"] = "Proj_Wyoming_CS27_West_Central";
                            } else {
                              fprintf(file_out, "ProjectionGeoKey: Proj_Wyoming_CS27_West_Central\012");
                            }
                            break;
                          case 14904:  // Proj_Wyoming_CS27_West
                            if (json_out) {
                              json_geo_key_entry["projection_geo_key"] = "Proj_Wyoming_CS27_West";
                            } else {
                              fprintf(file_out, "ProjectionGeoKey: Proj_Wyoming_CS27_West\012");
                            }
                            break;
                          case 14931:  // Proj_Wyoming_CS83_East
                            if (json_out) {
                              json_geo_key_entry["projection_geo_key"] = "Proj_Wyoming_CS83_East";
                            } else {
                              fprintf(file_out, "ProjectionGeoKey: Proj_Wyoming_CS83_East\012");
                            }
                            break;
                          case 14932:  // Proj_Wyoming_CS83_East_Central
                            if (json_out) {
                              json_geo_key_entry["projection_geo_key"] = "Proj_Wyoming_CS83_East_Central";
                            } else {
                              fprintf(file_out, "ProjectionGeoKey: Proj_Wyoming_CS83_East_Central\012");
                            }
                            break;
                          case 14933:  // Proj_Wyoming_CS83_West_Central
                            if (json_out) {
                              json_geo_key_entry["projection_geo_key"] = "Proj_Wyoming_CS83_West_Central";
                            } else {
                              fprintf(file_out, "ProjectionGeoKey: Proj_Wyoming_CS83_West_Central\012");
                            }
                            break;
                          case 14934:  // Proj_Wyoming_CS83_West
                            if (json_out) {
                              json_geo_key_entry["projection_geo_key"] = "Proj_Wyoming_CS83_West";
                            } else {
                              fprintf(file_out, "ProjectionGeoKey: Proj_Wyoming_CS83_West\012");
                            }
                            break;
                          case 15001:  // Proj_Alaska_CS27_1
                            if (json_out) {
                              json_geo_key_entry["projection_geo_key"] = "Proj_Alaska_CS27_1";
                            } else {
                              fprintf(file_out, "ProjectionGeoKey: Proj_Alaska_CS27_1\012");
                            }
                            break;
                          case 15002:  // Proj_Alaska_CS27_2
                            if (json_out) {
                              json_geo_key_entry["projection_geo_key"] = "Proj_Alaska_CS27_2";
                            } else {
                              fprintf(file_out, "ProjectionGeoKey: Proj_Alaska_CS27_2\012");
                            }
                            break;
                          case 15003:  // Proj_Alaska_CS27_3
                            if (json_out) {
                              json_geo_key_entry["projection_geo_key"] = "Proj_Alaska_CS27_3";
                            } else {
                              fprintf(file_out, "ProjectionGeoKey: Proj_Alaska_CS27_3\012");
                            }
                            break;
                          case 15004:  // Proj_Alaska_CS27_4
                            if (json_out) {
                              json_geo_key_entry["projection_geo_key"] = "Proj_Alaska_CS27_4";
                            } else {
                              fprintf(file_out, "ProjectionGeoKey: Proj_Alaska_CS27_4\012");
                            }
                            break;
                          case 15005:  // Proj_Alaska_CS27_5
                            if (json_out) {
                              json_geo_key_entry["projection_geo_key"] = "Proj_Alaska_CS27_5";
                            } else {
                              fprintf(file_out, "ProjectionGeoKey: Proj_Alaska_CS27_5\012");
                            }
                            break;
                          case 15006:  // Proj_Alaska_CS27_6
                            if (json_out) {
                              json_geo_key_entry["projection_geo_key"] = "Proj_Alaska_CS27_6";
                            } else {
                              fprintf(file_out, "ProjectionGeoKey: Proj_Alaska_CS27_6\012");
                            }
                            break;
                          case 15007:  // Proj_Alaska_CS27_7
                            if (json_out) {
                              json_geo_key_entry["projection_geo_key"] = "Proj_Alaska_CS27_7";
                            } else {
                              fprintf(file_out, "ProjectionGeoKey: Proj_Alaska_CS27_7\012");
                            }
                            break;
                          case 15008:  // Proj_Alaska_CS27_8
                            if (json_out) {
                              json_geo_key_entry["projection_geo_key"] = "Proj_Alaska_CS27_8";
                            } else {
                              fprintf(file_out, "ProjectionGeoKey: Proj_Alaska_CS27_8\012");
                            }
                            break;
                          case 15009:  // Proj_Alaska_CS27_9
                            if (json_out) {
                              json_geo_key_entry["projection_geo_key"] = "Proj_Alaska_CS27_9";
                            } else {
                              fprintf(file_out, "ProjectionGeoKey: Proj_Alaska_CS27_9\012");
                            }
                            break;
                          case 15010:  // Proj_Alaska_CS27_10
                            if (json_out) {
                              json_geo_key_entry["projection_geo_key"] = "Proj_Alaska_CS27_10";
                            } else {
                              fprintf(file_out, "ProjectionGeoKey: Proj_Alaska_CS27_10\012");
                            }
                            break;
                          case 15031:  // Proj_Alaska_CS83_1
                            if (json_out) {
                              json_geo_key_entry["projection_geo_key"] = "Proj_Alaska_CS83_1";
                            } else {
                              fprintf(file_out, "ProjectionGeoKey: Proj_Alaska_CS83_1\012");
                            }
                            break;
                          case 15032:  // Proj_Alaska_CS83_2
                            if (json_out) {
                              json_geo_key_entry["projection_geo_key"] = "Proj_Alaska_CS83_2";
                            } else {
                              fprintf(file_out, "ProjectionGeoKey: Proj_Alaska_CS83_2\012");
                            }
                            break;
                          case 15033:  // Proj_Alaska_CS83_3
                            if (json_out) {
                              json_geo_key_entry["projection_geo_key"] = "Proj_Alaska_CS83_3";
                            } else {
                              fprintf(file_out, "ProjectionGeoKey: Proj_Alaska_CS83_3\012");
                            }
                            break;
                          case 15034:  // Proj_Alaska_CS83_4
                            if (json_out) {
                              json_geo_key_entry["projection_geo_key"] = "Proj_Alaska_CS83_4";
                            } else {
                              fprintf(file_out, "ProjectionGeoKey: Proj_Alaska_CS83_4\012");
                            }
                            break;
                          case 15035:  // Proj_Alaska_CS83_5
                            if (json_out) {
                              json_geo_key_entry["projection_geo_key"] = "Proj_Alaska_CS83_5";
                            } else {
                              fprintf(file_out, "ProjectionGeoKey: Proj_Alaska_CS83_5\012");
                            }
                            break;
                          case 15036:  // Proj_Alaska_CS83_6
                            if (json_out) {
                              json_geo_key_entry["projection_geo_key"] = "Proj_Alaska_CS83_6";
                            } else {
                              fprintf(file_out, "ProjectionGeoKey: Proj_Alaska_CS83_6\012");
                            }
                            break;
                          case 15037:  // Proj_Alaska_CS83_7
                            if (json_out) {
                              json_geo_key_entry["projection_geo_key"] = "Proj_Alaska_CS83_7";
                            } else {
                              fprintf(file_out, "ProjectionGeoKey: Proj_Alaska_CS83_7\012");
                            }
                            break;
                          case 15038:  // Proj_Alaska_CS83_8
                            if (json_out) {
                              json_geo_key_entry["projection_geo_key"] = "Proj_Alaska_CS83_8";
                            } else {
                              fprintf(file_out, "ProjectionGeoKey: Proj_Alaska_CS83_8\012");
                            }
                            break;
                          case 15039:  // Proj_Alaska_CS83_9
                            if (json_out) {
                              json_geo_key_entry["projection_geo_key"] = "Proj_Alaska_CS83_9";
                            } else {
                              fprintf(file_out, "ProjectionGeoKey: Proj_Alaska_CS83_9\012");
                            }
                            break;
                          case 15040:  // Proj_Alaska_CS83_10
                            if (json_out) {
                              json_geo_key_entry["projection_geo_key"] = "Proj_Alaska_CS83_10";
                            } else {
                              fprintf(file_out, "ProjectionGeoKey: Proj_Alaska_CS83_10\012");
                            }
                            break;
                          case 15101:  // Proj_Hawaii_CS27_1
                            if (json_out) {
                              json_geo_key_entry["projection_geo_key"] = "Proj_Hawaii_CS27_1";
                            } else {
                              fprintf(file_out, "ProjectionGeoKey: Proj_Hawaii_CS27_1\012");
                            }
                            break;
                          case 15102:  // Proj_Hawaii_CS27_2
                            if (json_out) {
                              json_geo_key_entry["projection_geo_key"] = "Proj_Hawaii_CS27_2";
                            } else {
                              fprintf(file_out, "ProjectionGeoKey: Proj_Hawaii_CS27_2\012");
                            }
                            break;
                          case 15103:  // Proj_Hawaii_CS27_3
                            if (json_out) {
                              json_geo_key_entry["projection_geo_key"] = "Proj_Hawaii_CS27_3";
                            } else {
                              fprintf(file_out, "ProjectionGeoKey: Proj_Hawaii_CS27_3\012");
                            }
                            break;
                          case 15104:  // Proj_Hawaii_CS27_4
                            if (json_out) {
                              json_geo_key_entry["projection_geo_key"] = "Proj_Hawaii_CS27_4";
                            } else {
                              fprintf(file_out, "ProjectionGeoKey: Proj_Hawaii_CS27_4\012");
                            }
                            break;
                          case 15105:  // Proj_Hawaii_CS27_5
                            if (json_out) {
                              json_geo_key_entry["projection_geo_key"] = "Proj_Hawaii_CS27_5";
                            } else {
                              fprintf(file_out, "ProjectionGeoKey: Proj_Hawaii_CS27_5\012");
                            }
                            break;
                          case 15131:  // Proj_Hawaii_CS83_1
                            if (json_out) {
                              json_geo_key_entry["projection_geo_key"] = "Proj_Hawaii_CS83_1";
                            } else {
                              fprintf(file_out, "ProjectionGeoKey: Proj_Hawaii_CS83_1\012");
                            }
                            break;
                          case 15132:  // Proj_Hawaii_CS83_2
                            if (json_out) {
                              json_geo_key_entry["projection_geo_key"] = "Proj_Hawaii_CS83_2";
                            } else {
                              fprintf(file_out, "ProjectionGeoKey: Proj_Hawaii_CS83_2\012");
                            }
                            break;
                          case 15133:  // Proj_Hawaii_CS83_3
                            if (json_out) {
                              json_geo_key_entry["projection_geo_key"] = "Proj_Hawaii_CS83_3";
                            } else {
                              fprintf(file_out, "ProjectionGeoKey: Proj_Hawaii_CS83_3\012");
                            }
                            break;
                          case 15134:  // Proj_Hawaii_CS83_4
                            if (json_out) {
                              json_geo_key_entry["projection_geo_key"] = "Proj_Hawaii_CS83_4";
                            } else {
                              fprintf(file_out, "ProjectionGeoKey: Proj_Hawaii_CS83_4\012");
                            }
                            break;
                          case 15135:  // Proj_Hawaii_CS83_5
                            if (json_out) {
                              json_geo_key_entry["projection_geo_key"] = "Proj_Hawaii_CS83_5";
                            } else {
                              fprintf(file_out, "ProjectionGeoKey: Proj_Hawaii_CS83_5\012");
                            }
                            break;
                          case 15201:  // Proj_Puerto_Rico_CS27
                            if (json_out) {
                              json_geo_key_entry["projection_geo_key"] = "Proj_Puerto_Rico_CS27";
                            } else {
                              fprintf(file_out, "ProjectionGeoKey: Proj_Puerto_Rico_CS27\012");
                            }
                            break;
                          case 15202:  // Proj_St_Croix
                            if (json_out) {
                              json_geo_key_entry["projection_geo_key"] = "Proj_St_Croix";
                            } else {
                              fprintf(file_out, "ProjectionGeoKey: Proj_St_Croix\012");
                            }
                            break;
                          case 15230:  // Proj_Puerto_Rico_Virgin_Is
                            if (json_out) {
                              json_geo_key_entry["projection_geo_key"] = "Proj_Puerto_Rico_Virgin_Is";
                            } else {
                              fprintf(file_out, "ProjectionGeoKey: Proj_Puerto_Rico_Virgin_Is\012");
                            }
                            break;
                          case 15914:  // Proj_BLM_14N_feet
                            if (json_out) {
                              json_geo_key_entry["projection_geo_key"] = "Proj_BLM_14N_feet";
                            } else {
                              fprintf(file_out, "ProjectionGeoKey: Proj_BLM_14N_feet\012");
                            }
                            break;
                          case 15915:  // Proj_BLM_15N_feet
                            if (json_out) {
                              json_geo_key_entry["projection_geo_key"] = "Proj_BLM_15N_feet";
                            } else {
                              fprintf(file_out, "ProjectionGeoKey: Proj_BLM_15N_feet\012");
                            }
                            break;
                          case 15916:  // Proj_BLM_16N_feet
                            if (json_out) {
                              json_geo_key_entry["projection_geo_key"] = "Proj_BLM_16N_feet";
                            } else {
                              fprintf(file_out, "ProjectionGeoKey: Proj_BLM_16N_feet\012");
                            }
                            break;
                          case 15917:  // Proj_BLM_17N_feet
                            if (json_out) {
                              json_geo_key_entry["projection_geo_key"] = "Proj_BLM_17N_feet";
                            } else {
                              fprintf(file_out, "ProjectionGeoKey: Proj_BLM_17N_feet\012");
                            }
                            break;
                          case 17333:  // Proj_SWEREF99_TM
                            if (json_out) {
                              json_geo_key_entry["projection_geo_key"] = "Proj_SWEREF99_TM";
                            } else {
                              fprintf(file_out, "ProjectionGeoKey: Proj_SWEREF99_TM\012");
                            }
                            break;
                          case 17348:  // Proj_Map_Grid_of_Australia_48
                            if (json_out) {
                              json_geo_key_entry["projection_geo_key"] = "Proj_Map_Grid_of_Australia_48";
                            } else {
                              fprintf(file_out, "ProjectionGeoKey: Proj_Map_Grid_of_Australia_48\012");
                            }
                            break;
                          case 17349:  // Proj_Map_Grid_of_Australia_49
                            if (json_out) {
                              json_geo_key_entry["projection_geo_key"] = "Proj_Map_Grid_of_Australia_49";
                            } else {
                              fprintf(file_out, "ProjectionGeoKey: Proj_Map_Grid_of_Australia_49\012");
                            }
                            break;
                          case 17350:  // Proj_Map_Grid_of_Australia_50
                            if (json_out) {
                              json_geo_key_entry["projection_geo_key"] = "Proj_Map_Grid_of_Australia_50";
                            } else {
                              fprintf(file_out, "ProjectionGeoKey: Proj_Map_Grid_of_Australia_50\012");
                            }
                            break;
                          case 17351:  // Proj_Map_Grid_of_Australia_51
                            if (json_out) {
                              json_geo_key_entry["projection_geo_key"] = "Proj_Map_Grid_of_Australia_51";
                            } else {
                              fprintf(file_out, "ProjectionGeoKey: Proj_Map_Grid_of_Australia_51\012");
                            }
                            break;
                          case 17352:  // Proj_Map_Grid_of_Australia_52
                            if (json_out) {
                              json_geo_key_entry["projection_geo_key"] = "Proj_Map_Grid_of_Australia_52";
                            } else {
                              fprintf(file_out, "ProjectionGeoKey: Proj_Map_Grid_of_Australia_52\012");
                            }
                            break;
                          case 17353:  // Proj_Map_Grid_of_Australia_53
                            if (json_out) {
                              json_geo_key_entry["projection_geo_key"] = "Proj_Map_Grid_of_Australia_53";
                            } else {
                              fprintf(file_out, "ProjectionGeoKey: Proj_Map_Grid_of_Australia_53\012");
                            }
                            break;
                          case 17354:  // Proj_Map_Grid_of_Australia_54
                            if (json_out) {
                              json_geo_key_entry["projection_geo_key"] = "Proj_Map_Grid_of_Australia_54";
                            } else {
                              fprintf(file_out, "ProjectionGeoKey: Proj_Map_Grid_of_Australia_54\012");
                            }
                            break;
                          case 17355:  // Proj_Map_Grid_of_Australia_55
                            if (json_out) {
                              json_geo_key_entry["projection_geo_key"] = "Proj_Map_Grid_of_Australia_55";
                            } else {
                              fprintf(file_out, "ProjectionGeoKey: Proj_Map_Grid_of_Australia_55\012");
                            }
                            break;
                          case 17356:  // Proj_Map_Grid_of_Australia_56
                            if (json_out) {
                              json_geo_key_entry["projection_geo_key"] = "Proj_Map_Grid_of_Australia_56";
                            } else {
                              fprintf(file_out, "ProjectionGeoKey: Proj_Map_Grid_of_Australia_56\012");
                            }
                            break;
                          case 17357:  // Proj_Map_Grid_of_Australia_57
                            if (json_out) {
                              json_geo_key_entry["projection_geo_key"] = "Proj_Map_Grid_of_Australia_57";
                            } else {
                              fprintf(file_out, "ProjectionGeoKey: Proj_Map_Grid_of_Australia_57\012");
                            }
                            break;
                          case 17358:  // Proj_Map_Grid_of_Australia_58
                            if (json_out) {
                              json_geo_key_entry["projection_geo_key"] = "Proj_Map_Grid_of_Australia_58";
                            } else {
                              fprintf(file_out, "ProjectionGeoKey: Proj_Map_Grid_of_Australia_58\012");
                            }
                            break;
                          case 17448:  // Proj_Australian_Map_Grid_48
                            if (json_out) {
                              json_geo_key_entry["projection_geo_key"] = "Proj_Australian_Map_Grid_48";
                            } else {
                              fprintf(file_out, "ProjectionGeoKey: Proj_Australian_Map_Grid_48\012");
                            }
                            break;
                          case 17449:  // Proj_Australian_Map_Grid_49
                            if (json_out) {
                              json_geo_key_entry["projection_geo_key"] = "Proj_Australian_Map_Grid_49";
                            } else {
                              fprintf(file_out, "ProjectionGeoKey: Proj_Australian_Map_Grid_49\012");
                            }
                            break;
                          case 17450:  // Proj_Australian_Map_Grid_50
                            if (json_out) {
                              json_geo_key_entry["projection_geo_key"] = "Proj_Australian_Map_Grid_50";
                            } else {
                              fprintf(file_out, "ProjectionGeoKey: Proj_Australian_Map_Grid_50\012");
                            }
                            break;
                          case 17451:  // Proj_Australian_Map_Grid_51
                            if (json_out) {
                              json_geo_key_entry["projection_geo_key"] = "Proj_Australian_Map_Grid_51";
                            } else {
                              fprintf(file_out, "ProjectionGeoKey: Proj_Australian_Map_Grid_51\012");
                            }
                            break;
                          case 17452:  // Proj_Australian_Map_Grid_52
                            if (json_out) {
                              json_geo_key_entry["projection_geo_key"] = "Proj_Australian_Map_Grid_52";
                            } else {
                              fprintf(file_out, "ProjectionGeoKey: Proj_Australian_Map_Grid_52\012");
                            }
                            break;
                          case 17453:  // Proj_Australian_Map_Grid_53
                            if (json_out) {
                              json_geo_key_entry["projection_geo_key"] = "Proj_Australian_Map_Grid_53";
                            } else {
                              fprintf(file_out, "ProjectionGeoKey: Proj_Australian_Map_Grid_53\012");
                            }
                            break;
                          case 17454:  // Proj_Australian_Map_Grid_54
                            if (json_out) {
                              json_geo_key_entry["projection_geo_key"] = "Proj_Australian_Map_Grid_54";
                            } else {
                              fprintf(file_out, "ProjectionGeoKey: Proj_Australian_Map_Grid_54\012");
                            }
                            break;
                          case 17455:  // Proj_Australian_Map_Grid_55
                            if (json_out) {
                              json_geo_key_entry["projection_geo_key"] = "Proj_Australian_Map_Grid_55";
                            } else {
                              fprintf(file_out, "ProjectionGeoKey: Proj_Australian_Map_Grid_55\012");
                            }
                            break;
                          case 17456:  // Proj_Australian_Map_Grid_56
                            if (json_out) {
                              json_geo_key_entry["projection_geo_key"] = "Proj_Australian_Map_Grid_56";
                            } else {
                              fprintf(file_out, "ProjectionGeoKey: Proj_Australian_Map_Grid_56\012");
                            }
                            break;
                          case 17457:  // Proj_Australian_Map_Grid_57
                            if (json_out) {
                              json_geo_key_entry["projection_geo_key"] = "Proj_Australian_Map_Grid_57";
                            } else {
                              fprintf(file_out, "ProjectionGeoKey: Proj_Australian_Map_Grid_57\012");
                            }
                            break;
                          case 17458:  // Proj_Australian_Map_Grid_58
                            if (json_out) {
                              json_geo_key_entry["projection_geo_key"] = "Proj_Australian_Map_Grid_58";
                            } else {
                              fprintf(file_out, "ProjectionGeoKey: Proj_Australian_Map_Grid_58\012");
                            }
                            break;
                          case 18031:  // Proj_Argentina_1
                            if (json_out) {
                              json_geo_key_entry["projection_geo_key"] = "Proj_Argentina_1";
                            } else {
                              fprintf(file_out, "ProjectionGeoKey: Proj_Argentina_1\012");
                            }
                            break;
                          case 18032:  // Proj_Argentina_2
                            if (json_out) {
                              json_geo_key_entry["projection_geo_key"] = "Proj_Argentina_2";
                            } else {
                              fprintf(file_out, "ProjectionGeoKey: Proj_Argentina_2\012");
                            }
                            break;
                          case 18033:  // Proj_Argentina_3
                            if (json_out) {
                              json_geo_key_entry["projection_geo_key"] = "Proj_Argentina_3";
                            } else {
                              fprintf(file_out, "ProjectionGeoKey: Proj_Argentina_3\012");
                            }
                            break;
                          case 18034:  // Proj_Argentina_4
                            if (json_out) {
                              json_geo_key_entry["projection_geo_key"] = "Proj_Argentina_4";
                            } else {
                              fprintf(file_out, "ProjectionGeoKey: Proj_Argentina_4\012");
                            }
                            break;
                          case 18035:  // Proj_Argentina_5
                            if (json_out) {
                              json_geo_key_entry["projection_geo_key"] = "Proj_Argentina_5";
                            } else {
                              fprintf(file_out, "ProjectionGeoKey: Proj_Argentina_5\012");
                            }
                            break;
                          case 18036:  // Proj_Argentina_6
                            if (json_out) {
                              json_geo_key_entry["projection_geo_key"] = "Proj_Argentina_6";
                            } else {
                              fprintf(file_out, "ProjectionGeoKey: Proj_Argentina_6\012");
                            }
                            break;
                          case 18037:  // Proj_Argentina_7
                            if (json_out) {
                              json_geo_key_entry["projection_geo_key"] = "Proj_Argentina_7";
                            } else {
                              fprintf(file_out, "ProjectionGeoKey: Proj_Argentina_7\012");
                            }
                            break;
                          case 18051:  // Proj_Colombia_3W
                            if (json_out) {
                              json_geo_key_entry["projection_geo_key"] = "Proj_Colombia_3W";
                            } else {
                              fprintf(file_out, "ProjectionGeoKey: Proj_Colombia_3W\012");
                            }
                            break;
                          case 18052:  // Proj_Colombia_Bogota
                            if (json_out) {
                              json_geo_key_entry["projection_geo_key"] = "Proj_Colombia_Bogota";
                            } else {
                              fprintf(file_out, "ProjectionGeoKey: Proj_Colombia_Bogota\012");
                            }
                            break;
                          case 18053:  // Proj_Colombia_3E
                            if (json_out) {
                              json_geo_key_entry["projection_geo_key"] = "Proj_Colombia_3E";
                            } else {
                              fprintf(file_out, "ProjectionGeoKey: Proj_Colombia_3E\012");
                            }
                            break;
                          case 18054:  // Proj_Colombia_6E
                            if (json_out) {
                              json_geo_key_entry["projection_geo_key"] = "Proj_Colombia_6E";
                            } else {
                              fprintf(file_out, "ProjectionGeoKey: Proj_Colombia_6E\012");
                            }
                            break;
                          case 18072:  // Proj_Egypt_Red_Belt
                            if (json_out) {
                              json_geo_key_entry["projection_geo_key"] = "Proj_Egypt_Red_Belt";
                            } else {
                              fprintf(file_out, "ProjectionGeoKey: Proj_Egypt_Red_Belt\012");
                            }
                            break;
                          case 18073:  // Proj_Egypt_Purple_Belt
                            if (json_out) {
                              json_geo_key_entry["projection_geo_key"] = "Proj_Egypt_Purple_Belt";
                            } else {
                              fprintf(file_out, "ProjectionGeoKey: Proj_Egypt_Purple_Belt\012");
                            }
                            break;
                          case 18074:  // Proj_Extended_Purple_Belt
                            if (json_out) {
                              json_geo_key_entry["projection_geo_key"] = "Proj_Extended_Purple_Belt";
                            } else {
                              fprintf(file_out, "ProjectionGeoKey: Proj_Extended_Purple_Belt\012");
                            }
                            break;
                          case 18141:  // Proj_New_Zealand_North_Island_Nat_Grid
                            if (json_out) {
                              json_geo_key_entry["projection_geo_key"] = "Proj_New_Zealand_North_Island_Nat_Grid";
                            } else {
                              fprintf(file_out, "ProjectionGeoKey: Proj_New_Zealand_North_Island_Nat_Grid\012");
                            }
                            break;
                          case 18142:  // Proj_New_Zealand_South_Island_Nat_Grid
                            if (json_out) {
                              json_geo_key_entry["projection_geo_key"] = "Proj_New_Zealand_South_Island_Nat_Grid";
                            } else {
                              fprintf(file_out, "ProjectionGeoKey: Proj_New_Zealand_South_Island_Nat_Grid\012");
                            }
                            break;
                          case 19900:  // Proj_Bahrain_Grid
                            if (json_out) {
                              json_geo_key_entry["projection_geo_key"] = "Proj_Bahrain_Grid";
                            } else {
                              fprintf(file_out, "ProjectionGeoKey: Proj_Bahrain_Grid\012");
                            }
                            break;
                          case 19905:  // Proj_Netherlands_E_Indies_Equatorial
                            if (json_out) {
                              json_geo_key_entry["projection_geo_key"] = "Proj_Netherlands_E_Indies_Equatorial";
                            } else {
                              fprintf(file_out, "ProjectionGeoKey: Proj_Netherlands_E_Indies_Equatorial\012");
                            }
                            break;
                          case 19912:  // Proj_RSO_Borneo
                            if (json_out) {
                              json_geo_key_entry["projection_geo_key"] = "Proj_RSO_Borneo";
                            } else {
                              fprintf(file_out, "ProjectionGeoKey: Proj_RSO_Borneo\012");
                            }
                            break;
                          default:
                            if (json_out) {
                              char buffer[100];
                              snprintf(
                                  buffer, sizeof(buffer), "look-up for %d not implemented", lasreader->header.vlr_geo_key_entries[j].value_offset);
                              json_geo_key_entry["projection_geo_key"] = buffer;
                            } else {
                              fprintf(
                                  file_out, "ProjectionGeoKey: look-up for %d not implemented\012",
                                  lasreader->header.vlr_geo_key_entries[j].value_offset);
                            }
                        }
                      }
                      break;
                    case 3075:  // ProjCoordTransGeoKey
                      switch (lasreader->header.vlr_geo_key_entries[j].value_offset) {
                        case 1:  // CT_TransverseMercator
                          if (json_out) {
                            json_geo_key_entry["proj_coord_trans_geo_key"] = "CT_TransverseMercator";
                          } else {
                            fprintf(file_out, "ProjCoordTransGeoKey: CT_TransverseMercator\012");
                          }
                          break;
                        case 2:  // CT_TransvMercator_Modified_Alaska
                          if (json_out) {
                            json_geo_key_entry["proj_coord_trans_geo_key"] = "CT_TransvMercator_Modified_Alaska";
                          } else {
                            fprintf(file_out, "ProjCoordTransGeoKey: CT_TransvMercator_Modified_Alaska\012");
                          }
                          break;
                        case 3:  // CT_ObliqueMercator
                          if (json_out) {
                            json_geo_key_entry["proj_coord_trans_geo_key"] = "CT_ObliqueMercator";
                          } else {
                            fprintf(file_out, "ProjCoordTransGeoKey: CT_ObliqueMercator\012");
                          }
                          break;
                        case 4:  // CT_ObliqueMercator_Laborde
                          if (json_out) {
                            json_geo_key_entry["proj_coord_trans_geo_key"] = "CT_ObliqueMercator_Laborde";
                          } else {
                            fprintf(file_out, "ProjCoordTransGeoKey: CT_ObliqueMercator_Laborde\012");
                          }
                          break;
                        case 5:  // CT_ObliqueMercator_Rosenmund
                          if (json_out) {
                            json_geo_key_entry["proj_coord_trans_geo_key"] = "CT_ObliqueMercator_Rosenmund";
                          } else {
                            fprintf(file_out, "ProjCoordTransGeoKey: CT_ObliqueMercator_Rosenmund\012");
                          }
                          break;
                        case 6:  // CT_ObliqueMercator_Spherical
                          if (json_out) {
                            json_geo_key_entry["proj_coord_trans_geo_key"] = "CT_ObliqueMercator_Spherical";
                          } else {
                            fprintf(file_out, "ProjCoordTransGeoKey: CT_ObliqueMercator_Spherical\012");
                          }
                          break;
                        case 7:  // CT_Mercator
                          if (json_out) {
                            json_geo_key_entry["proj_coord_trans_geo_key"] = "CT_Mercator";
                          } else {
                            fprintf(file_out, "ProjCoordTransGeoKey: CT_Mercator\012");
                          }
                          break;
                        case 8:  // CT_LambertConfConic_2SP
                          if (json_out) {
                            json_geo_key_entry["proj_coord_trans_geo_key"] = "CT_LambertConfConic_2SP";
                          } else {
                            fprintf(file_out, "ProjCoordTransGeoKey: CT_LambertConfConic_2SP\012");
                          }
                          break;
                        case 9:  // CT_LambertConfConic_Helmert
                          if (json_out) {
                            json_geo_key_entry["proj_coord_trans_geo_key"] = "CT_LambertConfConic_Helmert";
                          } else {
                            fprintf(file_out, "ProjCoordTransGeoKey: CT_LambertConfConic_Helmert\012");
                          }
                          break;
                        case 10:  // CT_LambertAzimEqualArea
                          if (json_out) {
                            json_geo_key_entry["proj_coord_trans_geo_key"] = "CT_LambertAzimEqualArea";
                          } else {
                            fprintf(file_out, "ProjCoordTransGeoKey: CT_LambertAzimEqualArea\012");
                          }
                          break;
                        case 11:  // CT_AlbersEqualArea
                          if (json_out) {
                            json_geo_key_entry["proj_coord_trans_geo_key"] = "CT_AlbersEqualArea";
                          } else {
                            fprintf(file_out, "ProjCoordTransGeoKey: CT_AlbersEqualArea\012");
                          }
                          break;
                        case 12:  // CT_AzimuthalEquidistant
                          if (json_out) {
                            json_geo_key_entry["proj_coord_trans_geo_key"] = "CT_AzimuthalEquidistant";
                          } else {
                            fprintf(file_out, "ProjCoordTransGeoKey: CT_AzimuthalEquidistant\012");
                          }
                          break;
                        case 13:  // CT_EquidistantConic
                          if (json_out) {
                            json_geo_key_entry["proj_coord_trans_geo_key"] = "CT_EquidistantConic";
                          } else {
                            fprintf(file_out, "ProjCoordTransGeoKey: CT_EquidistantConic\012");
                          }
                          break;
                        case 14:  // CT_Stereographic
                          if (json_out) {
                            json_geo_key_entry["proj_coord_trans_geo_key"] = "CT_Stereographic";
                          } else {
                            fprintf(file_out, "ProjCoordTransGeoKey: CT_Stereographic\012");
                          }
                          break;
                        case 15:  // CT_PolarStereographic
                          if (json_out) {
                            json_geo_key_entry["proj_coord_trans_geo_key"] = "CT_PolarStereographic";
                          } else {
                            fprintf(file_out, "ProjCoordTransGeoKey: CT_PolarStereographic\012");
                          }
                          break;
                        case 16:  // CT_ObliqueStereographic
                          if (json_out) {
                            json_geo_key_entry["proj_coord_trans_geo_key"] = "CT_ObliqueStereographic";
                          } else {
                            fprintf(file_out, "ProjCoordTransGeoKey: CT_ObliqueStereographic\012");
                          }
                          break;
                        case 17:  // CT_Equirectangular
                          if (json_out) {
                            json_geo_key_entry["proj_coord_trans_geo_key"] = "CT_Equirectangular";
                          } else {
                            fprintf(file_out, "ProjCoordTransGeoKey: CT_Equirectangular\012");
                          }
                          break;
                        case 18:  // CT_CassiniSoldner
                          if (json_out) {
                            json_geo_key_entry["proj_coord_trans_geo_key"] = "CT_CassiniSoldner";
                          } else {
                            fprintf(file_out, "ProjCoordTransGeoKey: CT_CassiniSoldner\012");
                          }
                          break;
                        case 19:  // CT_Gnomonic
                          if (json_out) {
                            json_geo_key_entry["proj_coord_trans_geo_key"] = "CT_Gnomonic";
                          } else {
                            fprintf(file_out, "ProjCoordTransGeoKey: CT_Gnomonic\012");
                          }
                          break;
                        case 20:  // CT_MillerCylindrical
                          if (json_out) {
                            json_geo_key_entry["proj_coord_trans_geo_key"] = "CT_MillerCylindrical";
                          } else {
                            fprintf(file_out, "ProjCoordTransGeoKey: CT_MillerCylindrical\012");
                          }
                          break;
                        case 21:  // CT_Orthographic
                          if (json_out) {
                            json_geo_key_entry["proj_coord_trans_geo_key"] = "CT_Orthographic";
                          } else {
                            fprintf(file_out, "ProjCoordTransGeoKey: CT_Orthographic\012");
                          }
                          break;
                        case 22:  // CT_Polyconic
                          if (json_out) {
                            json_geo_key_entry["proj_coord_trans_geo_key"] = "CT_Polyconic";
                          } else {
                            fprintf(file_out, "ProjCoordTransGeoKey: CT_Polyconic\012");
                          }
                          break;
                        case 23:  // CT_Robinson
                          if (json_out) {
                            json_geo_key_entry["proj_coord_trans_geo_key"] = "CT_Robinson";
                          } else {
                            fprintf(file_out, "ProjCoordTransGeoKey: CT_Robinson\012");
                          }
                          break;
                        case 24:  // CT_Sinusoidal
                          if (json_out) {
                            json_geo_key_entry["proj_coord_trans_geo_key"] = "CT_Sinusoidal";
                          } else {
                            fprintf(file_out, "ProjCoordTransGeoKey: CT_Sinusoidal\012");
                          }
                          break;
                        case 25:  // CT_VanDerGrinten
                          if (json_out) {
                            json_geo_key_entry["proj_coord_trans_geo_key"] = "CT_VanDerGrinten";
                          } else {
                            fprintf(file_out, "ProjCoordTransGeoKey: CT_VanDerGrinten\012");
                          }
                          break;
                        case 26:  // CT_NewZealandMapGrid
                          if (json_out) {
                            json_geo_key_entry["proj_coord_trans_geo_key"] = "CT_NewZealandMapGrid";
                          } else {
                            fprintf(file_out, "ProjCoordTransGeoKey: CT_NewZealandMapGrid\012");
                          }
                          break;
                        case 27:  // CT_TransvMercator_SouthOriented
                          if (json_out) {
                            json_geo_key_entry["proj_coord_trans_geo_key"] = "CT_TransvMercator_SouthOriented";
                          } else {
                            fprintf(file_out, "ProjCoordTransGeoKey: CT_TransvMercator_SouthOriented\012");
                          }
                          break;
                        default:
                          if (json_out) {
                            char buffer[100];
                            snprintf(buffer, sizeof(buffer), "look-up for %d not implemented", lasreader->header.vlr_geo_key_entries[j].value_offset);
                            json_geo_key_entry["proj_coord_trans_geo_key"] = buffer;
                          } else {
                            fprintf(
                                file_out, "ProjCoordTransGeoKey: look-up for %d not implemented\012",
                                lasreader->header.vlr_geo_key_entries[j].value_offset);
                          }
                      }
                      break;
                    case 3076:  // ProjLinearUnitsGeoKey
                      horizontal_units = lasreader->header.vlr_geo_key_entries[j].value_offset;
                      switch (lasreader->header.vlr_geo_key_entries[j].value_offset) {
                        case 9001:  // Linear_Meter
                          if (json_out) {
                            json_geo_key_entry["proj_linear_units_geo_key"] = "Linear_Meter";
                          } else {
                            fprintf(file_out, "ProjLinearUnitsGeoKey: Linear_Meter\012");
                          }
                          break;
                        case 9002:  // Linear_Foot
                          if (json_out) {
                            json_geo_key_entry["proj_linear_units_geo_key"] = "Linear_Foot";
                          } else {
                            fprintf(file_out, "ProjLinearUnitsGeoKey: Linear_Foot\012");
                          }
                          break;
                        case 9003:  // Linear_Foot_US_Survey
                          if (json_out) {
                            json_geo_key_entry["proj_linear_units_geo_key"] = "Linear_Foot_US_Survey";
                          } else {
                            fprintf(file_out, "ProjLinearUnitsGeoKey: Linear_Foot_US_Survey\012");
                          }
                          break;
                        case 9004:  // Linear_Foot_Modified_American
                          if (json_out) {
                            json_geo_key_entry["proj_linear_units_geo_key"] = "Linear_Foot_Modified_American";
                          } else {
                            fprintf(file_out, "ProjLinearUnitsGeoKey: Linear_Foot_Modified_American\012");
                          }
                          break;
                        case 9005:  // Linear_Foot_Clarke
                          if (json_out) {
                            json_geo_key_entry["proj_linear_units_geo_key"] = "Linear_Foot_Clarke";
                          } else {
                            fprintf(file_out, "ProjLinearUnitsGeoKey: Linear_Foot_Clarke\012");
                          }
                          break;
                        case 9006:  // Linear_Foot_Indian
                          if (json_out) {
                            json_geo_key_entry["proj_linear_units_geo_key"] = "Linear_Foot_Indian";
                          } else {
                            fprintf(file_out, "ProjLinearUnitsGeoKey: Linear_Foot_Indian\012");
                          }
                          break;
                        case 9007:  // Linear_Link
                          if (json_out) {
                            json_geo_key_entry["proj_linear_units_geo_key"] = "Linear_Link";
                          } else {
                            fprintf(file_out, "ProjLinearUnitsGeoKey: Linear_Link\012");
                          }
                          break;
                        case 9008:  // Linear_Link_Benoit
                          if (json_out) {
                            json_geo_key_entry["proj_linear_units_geo_key"] = "Linear_Link_Benoit";
                          } else {
                            fprintf(file_out, "ProjLinearUnitsGeoKey: Linear_Link_Benoit\012");
                          }
                          break;
                        case 9009:  // Linear_Link_Sears
                          if (json_out) {
                            json_geo_key_entry["proj_linear_units_geo_key"] = "Linear_Link_Sears";
                          } else {
                            fprintf(file_out, "ProjLinearUnitsGeoKey: Linear_Link_Sears\012");
                          }
                          break;
                        case 9010:  // Linear_Chain_Benoit
                          if (json_out) {
                            json_geo_key_entry["proj_linear_units_geo_key"] = "Linear_Chain_Benoit";
                          } else {
                            fprintf(file_out, "ProjLinearUnitsGeoKey: Linear_Chain_Benoit\012");
                          }
                          break;
                        case 9011:  // Linear_Chain_Sears
                          if (json_out) {
                            json_geo_key_entry["proj_linear_units_geo_key"] = "Linear_Chain_Sears";
                          } else {
                            fprintf(file_out, "ProjLinearUnitsGeoKey: Linear_Chain_Sears\012");
                          }
                          break;
                        case 9012:  // Linear_Yard_Sears
                          if (json_out) {
                            json_geo_key_entry["proj_linear_units_geo_key"] = "Linear_Yard_Sears";
                          } else {
                            fprintf(file_out, "ProjLinearUnitsGeoKey: Linear_Yard_Sears\012");
                          }
                          break;
                        case 9013:  // Linear_Yard_Indian
                          if (json_out) {
                            json_geo_key_entry["proj_linear_units_geo_key"] = "Linear_Yard_Indian";
                          } else {
                            fprintf(file_out, "ProjLinearUnitsGeoKey: Linear_Yard_Indian\012");
                          }
                          break;
                        case 9014:  // Linear_Fathom
                          if (json_out) {
                            json_geo_key_entry["proj_linear_units_geo_key"] = "Linear_Fathom";
                          } else {
                            fprintf(file_out, "ProjLinearUnitsGeoKey: Linear_Fathom\012");
                          }
                          break;
                        case 9015:  // Linear_Mile_International_Nautical
                          if (json_out) {
                            json_geo_key_entry["proj_linear_units_geo_key"] = "Linear_Mile_International_Nautical";
                          } else {
                            fprintf(file_out, "ProjLinearUnitsGeoKey: Linear_Mile_International_Nautical\012");
                          }
                          break;
                        default:
                          if (json_out) {
                            char buffer[100];
                            snprintf(buffer, sizeof(buffer), "look-up for %d not implemented", lasreader->header.vlr_geo_key_entries[j].value_offset);
                            json_geo_key_entry["proj_linear_units_geo_key"] = buffer;
                          } else {
                            fprintf(
                                file_out, "ProjLinearUnitsGeoKey: look-up for %d not implemented\012",
                                lasreader->header.vlr_geo_key_entries[j].value_offset);
                          }
                      }
                      break;
                    case 3077:  // ProjLinearUnitSizeGeoKey
                      if (lasreader->header.vlr_geo_double_params) {
                        if (json_out) {
                          json_geo_key_entry["proj_linear_unit_size_geo_key"] =
                              round_to_decimals(lasreader->header.vlr_geo_double_params[lasreader->header.vlr_geo_key_entries[j].value_offset], 10);
                        } else {
                          fprintf(
                              file_out, "ProjLinearUnitSizeGeoKey: %.10g\012",
                              lasreader->header.vlr_geo_double_params[lasreader->header.vlr_geo_key_entries[j].value_offset]);
                        }
                      }
                      break;
                    case 3078:  // ProjStdParallel1GeoKey
                      if (lasreader->header.vlr_geo_double_params) {
                        if (json_out) {
                          json_geo_key_entry["proj_std_parallel1_geo_key"] =
                              round_to_decimals(lasreader->header.vlr_geo_double_params[lasreader->header.vlr_geo_key_entries[j].value_offset], 10);
                        } else {
                          fprintf(
                              file_out, "ProjStdParallel1GeoKey: %.10g\012",
                              lasreader->header.vlr_geo_double_params[lasreader->header.vlr_geo_key_entries[j].value_offset]);
                        }
                      }
                      break;
                    case 3079:  // ProjStdParallel2GeoKey
                      if (lasreader->header.vlr_geo_double_params) {
                        if (json_out) {
                          json_geo_key_entry["proj_std_parallel2_geo_key"] =
                              round_to_decimals(lasreader->header.vlr_geo_double_params[lasreader->header.vlr_geo_key_entries[j].value_offset], 10);
                        } else {
                          fprintf(
                              file_out, "ProjStdParallel2GeoKey: %.10g\012",
                              lasreader->header.vlr_geo_double_params[lasreader->header.vlr_geo_key_entries[j].value_offset]);
                        }
                      }
                      break;
                    case 3080:  // ProjNatOriginLongGeoKey
                      if (lasreader->header.vlr_geo_double_params) {
                        if (json_out) {
                          json_geo_key_entry["proj_nat_origin_long_geo_key"] =
                              round_to_decimals(lasreader->header.vlr_geo_double_params[lasreader->header.vlr_geo_key_entries[j].value_offset], 10);
                        } else {
                          fprintf(
                              file_out, "ProjNatOriginLongGeoKey: %.10g\012",
                              lasreader->header.vlr_geo_double_params[lasreader->header.vlr_geo_key_entries[j].value_offset]);
                        }
                      }
                      break;
                    case 3081:  // ProjNatOriginLatGeoKey
                      if (lasreader->header.vlr_geo_double_params) {
                        if (json_out) {
                          json_geo_key_entry["proj_nat_origin_lat_geo_key"] =
                              round_to_decimals(lasreader->header.vlr_geo_double_params[lasreader->header.vlr_geo_key_entries[j].value_offset], 10);
                        } else {
                          fprintf(
                              file_out, "ProjNatOriginLatGeoKey: %.10g\012",
                              lasreader->header.vlr_geo_double_params[lasreader->header.vlr_geo_key_entries[j].value_offset]);
                        }
                      }
                      break;
                    case 3082:  // ProjFalseEastingGeoKey
                      if (lasreader->header.vlr_geo_double_params) {
                        if (json_out) {
                          json_geo_key_entry["proj_false_easting_geo_key"] =
                              round_to_decimals(lasreader->header.vlr_geo_double_params[lasreader->header.vlr_geo_key_entries[j].value_offset], 10);
                        } else {
                          fprintf(
                              file_out, "ProjFalseEastingGeoKey: %.10g\012",
                              lasreader->header.vlr_geo_double_params[lasreader->header.vlr_geo_key_entries[j].value_offset]);
                        }
                      }
                      break;
                    case 3083:  // ProjFalseNorthingGeoKey
                      if (lasreader->header.vlr_geo_double_params) {
                        if (json_out) {
                          json_geo_key_entry["proj_false_northing_geo_key"] =
                              round_to_decimals(lasreader->header.vlr_geo_double_params[lasreader->header.vlr_geo_key_entries[j].value_offset], 10);
                        } else {
                          fprintf(
                              file_out, "ProjFalseNorthingGeoKey: %.10g\012",
                              lasreader->header.vlr_geo_double_params[lasreader->header.vlr_geo_key_entries[j].value_offset]);
                        }
                      }
                      break;
                    case 3084:  // ProjFalseOriginLongGeoKey
                      if (lasreader->header.vlr_geo_double_params) {
                        if (json_out) {
                          json_geo_key_entry["proj_false_origin_long_geo_key"] =
                              round_to_decimals(lasreader->header.vlr_geo_double_params[lasreader->header.vlr_geo_key_entries[j].value_offset], 10);
                        } else {
                          fprintf(
                              file_out, "ProjFalseOriginLongGeoKey: %.10g\012",
                              lasreader->header.vlr_geo_double_params[lasreader->header.vlr_geo_key_entries[j].value_offset]);
                        }
                      }
                      break;
                    case 3085:  // ProjFalseOriginLatGeoKey
                      if (lasreader->header.vlr_geo_double_params) {
                        if (json_out) {
                          json_geo_key_entry["proj_false_origin_lat_geo_key"] =
                              round_to_decimals(lasreader->header.vlr_geo_double_params[lasreader->header.vlr_geo_key_entries[j].value_offset], 10);
                        } else {
                          fprintf(
                              file_out, "ProjFalseOriginLatGeoKey: %.10g\012",
                              lasreader->header.vlr_geo_double_params[lasreader->header.vlr_geo_key_entries[j].value_offset]);
                        }
                      }
                      break;
                    case 3086:  // ProjFalseOriginEastingGeoKey
                      if (lasreader->header.vlr_geo_double_params) {
                        if (json_out) {
                          json_geo_key_entry["proj_false_origin_easting_geo_key"] =
                              round_to_decimals(lasreader->header.vlr_geo_double_params[lasreader->header.vlr_geo_key_entries[j].value_offset], 10);
                        } else {
                          fprintf(
                              file_out, "ProjFalseOriginEastingGeoKey: %.10g\012",
                              lasreader->header.vlr_geo_double_params[lasreader->header.vlr_geo_key_entries[j].value_offset]);
                        }
                      }
                      break;
                    case 3087:  // ProjFalseOriginNorthingGeoKey
                      if (lasreader->header.vlr_geo_double_params) {
                        if (json_out) {
                          json_geo_key_entry["proj_false_origin_northing_geo_key"] =
                              round_to_decimals(lasreader->header.vlr_geo_double_params[lasreader->header.vlr_geo_key_entries[j].value_offset], 10);
                        } else {
                          fprintf(
                              file_out, "ProjFalseOriginNorthingGeoKey: %.10g\012",
                              lasreader->header.vlr_geo_double_params[lasreader->header.vlr_geo_key_entries[j].value_offset]);
                        }
                      }
                      break;
                    case 3088:  // ProjCenterLongGeoKey
                      if (lasreader->header.vlr_geo_double_params) {
                        if (json_out) {
                          json_geo_key_entry["proj_center_long_geo_key"] =
                              round_to_decimals(lasreader->header.vlr_geo_double_params[lasreader->header.vlr_geo_key_entries[j].value_offset], 10);
                        } else {
                          fprintf(
                              file_out, "ProjCenterLongGeoKey: %.10g\012",
                              lasreader->header.vlr_geo_double_params[lasreader->header.vlr_geo_key_entries[j].value_offset]);
                        }
                      }
                      break;
                    case 3089:  // ProjCenterLatGeoKey
                      if (lasreader->header.vlr_geo_double_params) {
                        if (json_out) {
                          json_geo_key_entry["proj_center_lat_geo_key"] =
                              round_to_decimals(lasreader->header.vlr_geo_double_params[lasreader->header.vlr_geo_key_entries[j].value_offset], 10);
                        } else {
                          fprintf(
                              file_out, "ProjCenterLatGeoKey: %.10g\012",
                              lasreader->header.vlr_geo_double_params[lasreader->header.vlr_geo_key_entries[j].value_offset]);
                        }
                      }
                      break;
                    case 3090:  // ProjCenterEastingGeoKey
                      if (lasreader->header.vlr_geo_double_params) {
                        if (json_out) {
                          json_geo_key_entry["proj_center_easting_geo_key"] =
                              round_to_decimals(lasreader->header.vlr_geo_double_params[lasreader->header.vlr_geo_key_entries[j].value_offset], 10);
                        } else {
                          fprintf(
                              file_out, "ProjCenterEastingGeoKey: %.10g\012",
                              lasreader->header.vlr_geo_double_params[lasreader->header.vlr_geo_key_entries[j].value_offset]);
                        }
                      }
                      break;
                    case 3091:  // ProjCenterNorthingGeoKey
                      if (lasreader->header.vlr_geo_double_params) {
                        if (json_out) {
                          json_geo_key_entry["proj_center_northing_geo_key"] =
                              round_to_decimals(lasreader->header.vlr_geo_double_params[lasreader->header.vlr_geo_key_entries[j].value_offset], 10);
                        } else {
                          fprintf(
                              file_out, "ProjCenterNorthingGeoKey: %.10g\012",
                              lasreader->header.vlr_geo_double_params[lasreader->header.vlr_geo_key_entries[j].value_offset]);
                        }
                      }
                      break;
                    case 3092:  // ProjScaleAtNatOriginGeoKey
                      if (lasreader->header.vlr_geo_double_params) {
                        if (json_out) {
                          json_geo_key_entry["proj_scale_at_nat_origin_geo_key"] =
                              round_to_decimals(lasreader->header.vlr_geo_double_params[lasreader->header.vlr_geo_key_entries[j].value_offset], 10);
                        } else {
                          fprintf(
                              file_out, "ProjScaleAtNatOriginGeoKey: %.10g\012",
                              lasreader->header.vlr_geo_double_params[lasreader->header.vlr_geo_key_entries[j].value_offset]);
                        }
                      }
                      break;
                    case 3093:  // ProjScaleAtCenterGeoKey
                      if (lasreader->header.vlr_geo_double_params) {
                        if (json_out) {
                          json_geo_key_entry["proj_scale_at_center_geo_key"] =
                              round_to_decimals(lasreader->header.vlr_geo_double_params[lasreader->header.vlr_geo_key_entries[j].value_offset], 10);
                        } else {
                          fprintf(
                              file_out, "ProjScaleAtCenterGeoKey: %.10g\012",
                              lasreader->header.vlr_geo_double_params[lasreader->header.vlr_geo_key_entries[j].value_offset]);
                        }
                      }
                      break;
                    case 3094:  // ProjAzimuthAngleGeoKey
                      if (lasreader->header.vlr_geo_double_params) {
                        if (json_out) {
                          json_geo_key_entry["proj_azimuth_angle_geo_key"] =
                              round_to_decimals(lasreader->header.vlr_geo_double_params[lasreader->header.vlr_geo_key_entries[j].value_offset], 10);
                        } else {
                          fprintf(
                              file_out, "ProjAzimuthAngleGeoKey: %.10g\012",
                              lasreader->header.vlr_geo_double_params[lasreader->header.vlr_geo_key_entries[j].value_offset]);
                        }
                      }
                      break;
                    case 3095:  // ProjStraightVertPoleLongGeoKey
                      if (lasreader->header.vlr_geo_double_params) {
                        if (json_out) {
                          json_geo_key_entry["proj_straight_vert_pole_long_geo_key"] =
                              round_to_decimals(lasreader->header.vlr_geo_double_params[lasreader->header.vlr_geo_key_entries[j].value_offset], 10);
                        } else {
                          fprintf(
                              file_out, "ProjStraightVertPoleLongGeoKey: %.10g\012",
                              lasreader->header.vlr_geo_double_params[lasreader->header.vlr_geo_key_entries[j].value_offset]);
                        }
                      }
                      break;
                    case 4096:  // VerticalCSTypeGeoKey
                      switch (lasreader->header.vlr_geo_key_entries[j].value_offset) {
                        case 1127:  // VertCS_Canadian_Geodetic_Vertical_Datum_2013
                          if (json_out) {
                            json_geo_key_entry["vertical_cs_type_geo_key"] = "VertCS_Canadian_Geodetic_Vertical_Datum_2013";
                          } else {
                            fprintf(file_out, "VerticalCSTypeGeoKey: VertCS_Canadian_Geodetic_Vertical_Datum_2013\012");
                          }
                          break;
                        case 5001:  // VertCS_Airy_1830_ellipsoid
                          if (json_out) {
                            json_geo_key_entry["vertical_cs_type_geo_key"] = "VertCS_Airy_1830_ellipsoid";
                          } else {
                            fprintf(file_out, "VerticalCSTypeGeoKey: VertCS_Airy_1830_ellipsoid\012");
                          }
                          break;
                        case 5002:  // VertCS_Airy_Modified_1849_ellipsoid
                          if (json_out) {
                            json_geo_key_entry["vertical_cs_type_geo_key"] = "VertCS_Airy_Modified_1849_ellipsoid";
                          } else {
                            fprintf(file_out, "VerticalCSTypeGeoKey: VertCS_Airy_Modified_1849_ellipsoid\012");
                          }
                          break;
                        case 5003:  // VertCS_ANS_ellipsoid
                          if (json_out) {
                            json_geo_key_entry["vertical_cs_type_geo_key"] = "VertCS_ANS_ellipsoid";
                          } else {
                            fprintf(file_out, "VerticalCSTypeGeoKey: VertCS_ANS_ellipsoid\012");
                          }
                          break;
                        case 5004:  // VertCS_Bessel_1841_ellipsoid
                          if (json_out) {
                            json_geo_key_entry["vertical_cs_type_geo_key"] = "VertCS_Bessel_1841_ellipsoid";
                          } else {
                            fprintf(file_out, "VerticalCSTypeGeoKey: VertCS_Bessel_1841_ellipsoid\012");
                          }
                          break;
                        case 5005:  // VertCS_Bessel_Modified_ellipsoid
                          if (json_out) {
                            json_geo_key_entry["vertical_cs_type_geo_key"] = "VertCS_Bessel_Modified_ellipsoid";
                          } else {
                            fprintf(file_out, "VerticalCSTypeGeoKey: VertCS_Bessel_Modified_ellipsoid\012");
                          }
                          break;
                        case 5006:  // VertCS_Bessel_Namibia_ellipsoid
                          if (json_out) {
                            json_geo_key_entry["vertical_cs_type_geo_key"] = "VertCS_Bessel_Namibia_ellipsoid";
                          } else {
                            fprintf(file_out, "VerticalCSTypeGeoKey: VertCS_Bessel_Namibia_ellipsoid\012");
                          }
                          break;
                        case 5007:  // VertCS_Clarke_1858_ellipsoid
                          if (json_out) {
                            json_geo_key_entry["vertical_cs_type_geo_key"] = "VertCS_Clarke_1858_ellipsoid";
                          } else {
                            fprintf(file_out, "VerticalCSTypeGeoKey: VertCS_Clarke_1858_ellipsoid\012");
                          }
                          break;
                        case 5008:  // VertCS_Clarke_1866_ellipsoid
                          if (json_out) {
                            json_geo_key_entry["vertical_cs_type_geo_key"] = "VertCS_Clarke_1866_ellipsoid";
                          } else {
                            fprintf(file_out, "VerticalCSTypeGeoKey: VertCS_Clarke_1866_ellipsoid\012");
                          }
                          break;
                        case 5010:  // VertCS_Clarke_1880_Benoit_ellipsoid
                          if (json_out) {
                            json_geo_key_entry["vertical_cs_type_geo_key"] = "VertCS_Clarke_1880_Benoit_ellipsoid";
                          } else {
                            fprintf(file_out, "VerticalCSTypeGeoKey: VertCS_Clarke_1880_Benoit_ellipsoid\012");
                          }
                          break;
                        case 5011:  // VertCS_Clarke_1880_IGN_ellipsoid
                          if (json_out) {
                            json_geo_key_entry["vertical_cs_type_geo_key"] = "VertCS_Clarke_1880_IGN_ellipsoid";
                          } else {
                            fprintf(file_out, "VerticalCSTypeGeoKey: VertCS_Clarke_1880_IGN_ellipsoid\012");
                          }
                          break;
                        case 5012:  // VertCS_Clarke_1880_RGS_ellipsoid
                          if (json_out) {
                            json_geo_key_entry["vertical_cs_type_geo_key"] = "VertCS_Clarke_1880_RGS_ellipsoid";
                          } else {
                            fprintf(file_out, "VerticalCSTypeGeoKey: VertCS_Clarke_1880_RGS_ellipsoid\012");
                          }
                          break;
                        case 5013:  // VertCS_Clarke_1880_Arc_ellipsoid
                          if (json_out) {
                            json_geo_key_entry["vertical_cs_type_geo_key"] = "VertCS_Clarke_1880_Arc_ellipsoid";
                          } else {
                            fprintf(file_out, "VerticalCSTypeGeoKey: VertCS_Clarke_1880_Arc_ellipsoid\012");
                          }
                          break;
                        case 5014:  // VertCS_Clarke_1880_SGA_1922_ellipsoid
                          if (json_out) {
                            json_geo_key_entry["vertical_cs_type_geo_key"] = "VertCS_Clarke_1880_SGA_1922_ellipsoid";
                          } else {
                            fprintf(file_out, "VerticalCSTypeGeoKey: VertCS_Clarke_1880_SGA_1922_ellipsoid\012");
                          }
                          break;
                        case 5015:  // VertCS_Everest_1830_1937_Adjustment_ellipsoid
                          if (json_out) {
                            json_geo_key_entry["vertical_cs_type_geo_key"] = "VertCS_Everest_1830_1937_Adjustment_ellipsoid";
                          } else {
                            fprintf(file_out, "VerticalCSTypeGeoKey: VertCS_Everest_1830_1937_Adjustment_ellipsoid\012");
                          }
                          break;
                        case 5016:  // VertCS_Everest_1830_1967_Definition_ellipsoid
                          if (json_out) {
                            json_geo_key_entry["vertical_cs_type_geo_key"] = "VertCS_Everest_1830_1967_Definition_ellipsoid";
                          } else {
                            fprintf(file_out, "VerticalCSTypeGeoKey: VertCS_Everest_1830_1967_Definition_ellipsoid\012");
                          }
                          break;
                        case 5017:  // VertCS_Everest_1830_1975_Definition_ellipsoid
                          if (json_out) {
                            json_geo_key_entry["vertical_cs_type_geo_key"] = "VertCS_Everest_1830_1975_Definition_ellipsoid";
                          } else {
                            fprintf(file_out, "VerticalCSTypeGeoKey: VertCS_Everest_1830_1975_Definition_ellipsoid\012");
                          }
                          break;
                        case 5018:  // VertCS_Everest_1830_Modified_ellipsoid
                          if (json_out) {
                            json_geo_key_entry["vertical_cs_type_geo_key"] = "VertCS_Everest_1830_Modified_ellipsoid";
                          } else {
                            fprintf(file_out, "VerticalCSTypeGeoKey: VertCS_Everest_1830_Modified_ellipsoid\012");
                          }
                          break;
                        case 5019:  // VertCS_GRS_1980_ellipsoid
                          if (json_out) {
                            json_geo_key_entry["vertical_cs_type_geo_key"] = "VertCS_GRS_1980_ellipsoid";
                          } else {
                            fprintf(file_out, "VerticalCSTypeGeoKey: VertCS_GRS_1980_ellipsoid\012");
                          }
                          break;
                        case 5020:  // VertCS_Helmert_1906_ellipsoid
                          if (json_out) {
                            json_geo_key_entry["vertical_cs_type_geo_key"] = "VertCS_Helmert_1906_ellipsoid";
                          } else {
                            fprintf(file_out, "VerticalCSTypeGeoKey: VertCS_Helmert_1906_ellipsoid\012");
                          }
                          break;
                        case 5021:  // VertCS_INS_ellipsoid
                          if (json_out) {
                            json_geo_key_entry["vertical_cs_type_geo_key"] = "VertCS_INS_ellipsoid";
                          } else {
                            fprintf(file_out, "VerticalCSTypeGeoKey: VertCS_INS_ellipsoid\012");
                          }
                          break;
                        case 5022:  // VertCS_International_1924_ellipsoid
                          if (json_out) {
                            json_geo_key_entry["vertical_cs_type_geo_key"] = "VertCS_International_1924_ellipsoid";
                          } else {
                            fprintf(file_out, "VerticalCSTypeGeoKey: VertCS_International_1924_ellipsoid\012");
                          }
                          break;
                        case 5023:  // VertCS_International_1967_ellipsoid
                          if (json_out) {
                            json_geo_key_entry["vertical_cs_type_geo_key"] = "VertCS_International_1967_ellipsoid";
                          } else {
                            fprintf(file_out, "VerticalCSTypeGeoKey: VertCS_International_1967_ellipsoid\012");
                          }
                          break;
                        case 5024:  // VertCS_Krassowsky_1940_ellipsoid
                          if (json_out) {
                            json_geo_key_entry["vertical_cs_type_geo_key"] = "VertCS_Krassowsky_1940_ellipsoid";
                          } else {
                            fprintf(file_out, "VerticalCSTypeGeoKey: VertCS_Krassowsky_1940_ellipsoid\012");
                          }
                          break;
                        case 5025:  // VertCS_NWL_9D_ellipsoid
                          if (json_out) {
                            json_geo_key_entry["vertical_cs_type_geo_key"] = "VertCS_NWL_9D_ellipsoid";
                          } else {
                            fprintf(file_out, "VerticalCSTypeGeoKey: VertCS_NWL_9D_ellipsoid\012");
                          }
                          break;
                        case 5026:  // VertCS_NWL_10D_ellipsoid
                          if (json_out) {
                            json_geo_key_entry["vertical_cs_type_geo_key"] = "VertCS_NWL_10D_ellipsoid";
                          } else {
                            fprintf(file_out, "VerticalCSTypeGeoKey: VertCS_NWL_10D_ellipsoid\012");
                          }
                          break;
                        case 5027:  // VertCS_Plessis_1817_ellipsoid
                          if (json_out) {
                            json_geo_key_entry["vertical_cs_type_geo_key"] = "VertCS_Plessis_1817_ellipsoid";
                          } else {
                            fprintf(file_out, "VerticalCSTypeGeoKey: VertCS_Plessis_1817_ellipsoid\012");
                          }
                          break;
                        case 5028:  // VertCS_Struve_1860_ellipsoid
                          if (json_out) {
                            json_geo_key_entry["vertical_cs_type_geo_key"] = "VertCS_Struve_1860_ellipsoid";
                          } else {
                            fprintf(file_out, "VerticalCSTypeGeoKey: VertCS_Struve_1860_ellipsoid\012");
                          }
                          break;
                        case 5029:  // VertCS_War_Office_ellipsoid
                          if (json_out) {
                            json_geo_key_entry["vertical_cs_type_geo_key"] = "VertCS_War_Office_ellipsoid";
                          } else {
                            fprintf(file_out, "VerticalCSTypeGeoKey: VertCS_War_Office_ellipsoid\012");
                          }
                          break;
                        case 5030:  // VertCS_WGS_84_ellipsoid
                          if (json_out) {
                            json_geo_key_entry["vertical_cs_type_geo_key"] = "VertCS_WGS_84_ellipsoid";
                          } else {
                            fprintf(file_out, "VerticalCSTypeGeoKey: VertCS_WGS_84_ellipsoid\012");
                          }
                          break;
                        case 5031:  // VertCS_GEM_10C_ellipsoid
                          if (json_out) {
                            json_geo_key_entry["vertical_cs_type_geo_key"] = "VertCS_GEM_10C_ellipsoid";
                          } else {
                            fprintf(file_out, "VerticalCSTypeGeoKey: VertCS_GEM_10C_ellipsoid\012");
                          }
                          break;
                        case 5032:  // VertCS_OSU86F_ellipsoid
                          if (json_out) {
                            json_geo_key_entry["vertical_cs_type_geo_key"] = "VertCS_OSU86F_ellipsoid";
                          } else {
                            fprintf(file_out, "VerticalCSTypeGeoKey: VertCS_OSU86F_ellipsoid\012");
                          }
                          break;
                        case 5033:  // VertCS_OSU91A_ellipsoid
                          if (json_out) {
                            json_geo_key_entry["vertical_cs_type_geo_key"] = "VertCS_OSU91A_ellipsoid";
                          } else {
                            fprintf(file_out, "VerticalCSTypeGeoKey: VertCS_OSU91A_ellipsoid\012");
                          }
                          break;
                        case 5101:  // VertCS_Newlyn
                          if (json_out) {
                            json_geo_key_entry["vertical_cs_type_geo_key"] = "VertCS_Newlyn";
                          } else {
                            fprintf(file_out, "VerticalCSTypeGeoKey: VertCS_Newlyn\012");
                          }
                          break;
                        case 5102:  // VertCS_North_American_Vertical_Datum_1929
                          if (json_out) {
                            json_geo_key_entry["vertical_cs_type_geo_key"] = "VertCS_North_American_Vertical_Datum_1929";
                          } else {
                            fprintf(file_out, "VerticalCSTypeGeoKey: VertCS_North_American_Vertical_Datum_1929\012");
                          }
                          break;
                        case 5103:  // VertCS_North_American_Vertical_Datum_1988
                          if (json_out) {
                            json_geo_key_entry["vertical_cs_type_geo_key"] = "VertCS_North_American_Vertical_Datum_1988";
                          } else {
                            fprintf(file_out, "VerticalCSTypeGeoKey: VertCS_North_American_Vertical_Datum_1988\012");
                          }
                          break;
                        case 5104:  // VertCS_Yellow_Sea_1956
                          if (json_out) {
                            json_geo_key_entry["vertical_cs_type_geo_key"] = "VertCS_Yellow_Sea_1956";
                          } else {
                            fprintf(file_out, "VerticalCSTypeGeoKey: VertCS_Yellow_Sea_1956\012");
                          }
                          break;
                        case 5105:  // VertCS_Baltic_Sea
                          if (json_out) {
                            json_geo_key_entry["vertical_cs_type_geo_key"] = "VertCS_Baltic_Sea";
                          } else {
                            fprintf(file_out, "VerticalCSTypeGeoKey: VertCS_Baltic_Sea\012");
                          }
                          break;
                        case 5106:  // VertCS_Caspian_Sea
                          if (json_out) {
                            json_geo_key_entry["vertical_cs_type_geo_key"] = "VertCS_Caspian_Sea";
                          } else {
                            fprintf(file_out, "VerticalCSTypeGeoKey: VertCS_Caspian_Sea\012");
                          }
                          break;
                        case 5114:  // VertCS_Canadian_Geodetic_Vertical_Datum_1928
                          if (json_out) {
                            json_geo_key_entry["vertical_cs_type_geo_key"] = "VertCS_Canadian_Geodetic_Vertical_Datum_1928";
                          } else {
                            fprintf(file_out, "VerticalCSTypeGeoKey: VertCS_Canadian_Geodetic_Vertical_Datum_1928\012");
                          }
                          break;
                        case 5206:  // VertCS_Dansk_Vertikal_Reference_1990
                          if (json_out) {
                            json_geo_key_entry["vertical_cs_type_geo_key"] = "VertCS_Dansk_Vertikal_Reference_1990";
                          } else {
                            fprintf(file_out, "VerticalCSTypeGeoKey: VertCS_Dansk_Vertikal_Reference_1990\012");
                          }
                          break;
                        case 5215:  // VertCS_European_Vertical_Reference_Frame_2007
                          if (json_out) {
                            json_geo_key_entry["vertical_cs_type_geo_key"] = "VertCS_European_Vertical_Reference_Frame_2007";
                          } else {
                            fprintf(file_out, "VerticalCSTypeGeoKey: VertCS_European_Vertical_Reference_Frame_2007\012");
                          }
                          break;
                        case 5701:  // ODN height (Reserved EPSG)
                          if (json_out) {
                            json_geo_key_entry["vertical_cs_type_geo_key"] = "ODN height (Reserved EPSG)";
                          } else {
                            fprintf(file_out, "VerticalCSTypeGeoKey: ODN height (Reserved EPSG)\012");
                          }
                          break;
                        case 5702:  // NGVD29 height (Reserved EPSG)
                          if (json_out) {
                            json_geo_key_entry["vertical_cs_type_geo_key"] = "NGVD29 height (Reserved EPSG)";
                          } else {
                            fprintf(file_out, "VerticalCSTypeGeoKey: NGVD29 height (Reserved EPSG)\012");
                          }
                          break;
                        case 5703:  // NAVD88 height (Reserved EPSG)
                          if (json_out) {
                            json_geo_key_entry["vertical_cs_type_geo_key"] = "NAVD88 height (Reserved EPSG)";
                          } else {
                            fprintf(file_out, "VerticalCSTypeGeoKey: NAVD88 height (Reserved EPSG)\012");
                          }
                          break;
                        case 5704:  // Yellow Sea (Reserved EPSG)
                          if (json_out) {
                            json_geo_key_entry["vertical_cs_type_geo_key"] = "Yellow Sea (Reserved EPSG)";
                          } else {
                            fprintf(file_out, "VerticalCSTypeGeoKey: Yellow Sea (Reserved EPSG)\012");
                          }
                          break;
                        case 5705:  // Baltic height (Reserved EPSG)
                          if (json_out) {
                            json_geo_key_entry["vertical_cs_type_geo_key"] = "Baltic height (Reserved EPSG)";
                          } else {
                            fprintf(file_out, "VerticalCSTypeGeoKey: Baltic height (Reserved EPSG)\012");
                          }
                          break;
                        case 5706:  // Caspian depth (Reserved EPSG)
                          if (json_out) {
                            json_geo_key_entry["vertical_cs_type_geo_key"] = "Caspian depth (Reserved EPSG)";
                          } else {
                            fprintf(file_out, "VerticalCSTypeGeoKey: Caspian depth (Reserved EPSG)\012");
                          }
                          break;
                        case 5707:  // NAP height (Reserved EPSG)
                          if (json_out) {
                            json_geo_key_entry["vertical_cs_type_geo_key"] = "NAP height (Reserved EPSG)";
                          } else {
                            fprintf(file_out, "VerticalCSTypeGeoKey: NAP height (Reserved EPSG)\012");
                          }
                          break;
                        case 5710:  // Oostende height (Reserved EPSG)
                          if (json_out) {
                            json_geo_key_entry["vertical_cs_type_geo_key"] = "Oostende height (Reserved EPSG)";
                          } else {
                            fprintf(file_out, "VerticalCSTypeGeoKey: Oostende height (Reserved EPSG)\012");
                          }
                          break;
                        case 5711:  // AHD height (Reserved EPSG)
                          if (json_out) {
                            json_geo_key_entry["vertical_cs_type_geo_key"] = "AHD height (Reserved EPSG)";
                          } else {
                            fprintf(file_out, "VerticalCSTypeGeoKey: AHD height (Reserved EPSG)\012");
                          }
                          break;
                        case 5712:  // AHD (Tasmania) height (Reserved EPSG)
                          if (json_out) {
                            json_geo_key_entry["vertical_cs_type_geo_key"] = "AHD (Tasmania) height (Reserved EPSG)";
                          } else {
                            fprintf(file_out, "VerticalCSTypeGeoKey: AHD (Tasmania) height (Reserved EPSG)\012");
                          }
                          break;
                        case 5776:  // Norway Normal Null 1954
                          if (json_out) {
                            json_geo_key_entry["vertical_cs_type_geo_key"] = "Norway Normal Null 1954";
                          } else {
                            fprintf(file_out, "VerticalCSTypeGeoKey: Norway Normal Null 1954\012");
                          }
                          break;
                        case 5783:  // Deutsches Haupthohennetz 1992
                          if (json_out) {
                            json_geo_key_entry["vertical_cs_type_geo_key"] = "Deutsches Haupthoehennetz 1992";
                          } else {
                            fprintf(file_out, "VerticalCSTypeGeoKey: Deutsches Haupthoehennetz 1992\012");
                          }
                          break;
                        case 5941:  // Norway Normal Null 2000
                          if (json_out) {
                            json_geo_key_entry["vertical_cs_type_geo_key"] = "Norway Normal Null 2000";
                          } else {
                            fprintf(file_out, "VerticalCSTypeGeoKey: Norway Normal Null 2000\012");
                          }
                          break;
                        case 6647:  // Canadian Geodetic Vertical Datum of 2013
                          if (json_out) {
                            json_geo_key_entry["vertical_cs_type_geo_key"] = "Canadian Geodetic Vertical Datum of 2013";
                          } else {
                            fprintf(file_out, "VerticalCSTypeGeoKey: Canadian Geodetic Vertical Datum of 2013\012");
                          }
                          break;
                        case 7837:  // Deutsches Haupthohennetz 2016
                          if (json_out) {
                            json_geo_key_entry["vertical_cs_type_geo_key"] = "Deutsches Haupthoehennetz 2016";
                          } else {
                            fprintf(file_out, "VerticalCSTypeGeoKey: Deutsches Haupthoehennetz 2016\012");
                          }
                          break;
                        default:
                          if (geoprojectionconverter.set_VerticalCSTypeGeoKey(lasreader->header.vlr_geo_key_entries[j].value_offset, printstring)) {
                            if (json_out) {
                              json_geo_key_entry["vertical_cs_type_geo_key"] = printstring;
                            } else {
                              fprintf(file_out, "VerticalCSTypeGeoKey: %s\012", printstring);
                            }
                          } else {
                            if (json_out) {
                              char buffer[100];
                              snprintf(
                                  buffer, sizeof(buffer), "look-up for %d not implemented", lasreader->header.vlr_geo_key_entries[j].value_offset);
                              json_geo_key_entry["vertical_cs_type_geo_key"] = buffer;
                            } else {
                              fprintf(
                                  file_out, "VerticalCSTypeGeoKey: look-up for %d not implemented\012",
                                  lasreader->header.vlr_geo_key_entries[j].value_offset);
                            }
                          }
                      }
                      break;
                    case 4097:  // VerticalCitationGeoKey
                      if (lasreader->header.vlr_geo_ascii_params) {
                        char dummy[256];
                        strncpy_las(
                            dummy, sizeof(dummy), &(lasreader->header.vlr_geo_ascii_params[lasreader->header.vlr_geo_key_entries[j].value_offset]),
                            lasreader->header.vlr_geo_key_entries[j].count);
                        dummy[lasreader->header.vlr_geo_key_entries[j].count - 1] = '\0';
                        if (json_out) {
                          json_geo_key_entry["vertical_citation_geo_key"] = dummy;
                        } else {
                          fprintf(file_out, "VerticalCitationGeoKey: %s\012", dummy);
                        }
                      }
                      break;
                    case 4098:  // VerticalDatumGeoKey
                      if (json_out) {
                        char buffer[100];  // Puffer ohne Initialisierung
                        snprintf(buffer, sizeof(buffer), "Vertical Datum Codes %d", lasreader->header.vlr_geo_key_entries[j].value_offset);
                        json_geo_key_entry["vertical_datum_geo_key"] = buffer;
                      } else {
                        fprintf(file_out, "VerticalDatumGeoKey: Vertical Datum Codes %d\012", lasreader->header.vlr_geo_key_entries[j].value_offset);
                      }
                      break;
                    case 4099:  // VerticalUnitsGeoKey
                      switch (lasreader->header.vlr_geo_key_entries[j].value_offset) {
                        case 9001:  // Linear_Meter
                          if (json_out) {
                            json_geo_key_entry["vertical_units_geo_key"] = "Linear_Meter";
                          } else {
                            fprintf(file_out, "VerticalUnitsGeoKey: Linear_Meter\012");
                          }
                          break;
                        case 9002:  // Linear_Foot
                          if (json_out) {
                            json_geo_key_entry["vertical_units_geo_key"] = "Linear_Foot";
                          } else {
                            fprintf(file_out, "VerticalUnitsGeoKey: Linear_Foot\012");
                          }
                          break;
                        case 9003:  // Linear_Foot_US_Survey
                          if (json_out) {
                            json_geo_key_entry["vertical_units_geo_key"] = "Linear_Foot_US_Survey";
                          } else {
                            fprintf(file_out, "VerticalUnitsGeoKey: Linear_Foot_US_Survey\012");
                          }
                          break;
                        case 9004:  // Linear_Foot_Modified_American
                          if (json_out) {
                            json_geo_key_entry["vertical_units_geo_key"] = "Linear_Foot_Modified_American";
                          } else {
                            fprintf(file_out, "VerticalUnitsGeoKey: Linear_Foot_Modified_American\012");
                          }
                          break;
                        case 9005:  // Linear_Foot_Clarke
                          if (json_out) {
                            json_geo_key_entry["vertical_units_geo_key"] = "Linear_Foot_Clarke";
                          } else {
                            fprintf(file_out, "VerticalUnitsGeoKey: Linear_Foot_Clarke\012");
                          }
                          break;
                        case 9006:  // Linear_Foot_Indian
                          if (json_out) {
                            json_geo_key_entry["vertical_units_geo_key"] = "Linear_Foot_Indian";
                          } else {
                            fprintf(file_out, "VerticalUnitsGeoKey: Linear_Foot_Indian\012");
                          }
                          break;
                        case 9007:  // Linear_Link
                          if (json_out) {
                            json_geo_key_entry["vertical_units_geo_key"] = "Linear_Link";
                          } else {
                            fprintf(file_out, "VerticalUnitsGeoKey: Linear_Link\012");
                          }
                          break;
                        case 9008:  // Linear_Link_Benoit
                          if (json_out) {
                            json_geo_key_entry["vertical_units_geo_key"] = "Linear_Link_Benoit";
                          } else {
                            fprintf(file_out, "VerticalUnitsGeoKey: Linear_Link_Benoit\012");
                          }
                          break;
                        case 9009:  // Linear_Link_Sears
                          if (json_out) {
                            json_geo_key_entry["vertical_units_geo_key"] = "Linear_Link_Sears";
                          } else {
                            fprintf(file_out, "VerticalUnitsGeoKey: Linear_Link_Sears\012");
                          }
                          break;
                        case 9010:  // Linear_Chain_Benoit
                          if (json_out) {
                            json_geo_key_entry["vertical_units_geo_key"] = "Linear_Chain_Benoit";
                          } else {
                            fprintf(file_out, "VerticalUnitsGeoKey: Linear_Chain_Benoit\012");
                          }
                          break;
                        case 9011:  // Linear_Chain_Sears
                          if (json_out) {
                            json_geo_key_entry["vertical_units_geo_key"] = "Linear_Chain_Sears";
                          } else {
                            fprintf(file_out, "VerticalUnitsGeoKey: Linear_Chain_Sears\012");
                          }
                          break;
                        case 9012:  // Linear_Yard_Sears
                          if (json_out) {
                            json_geo_key_entry["vertical_units_geo_key"] = "Linear_Yard_Sears";
                          } else {
                            fprintf(file_out, "VerticalUnitsGeoKey: Linear_Yard_Sears\012");
                          }
                          break;
                        case 9013:  // Linear_Yard_Indian
                          if (json_out) {
                            json_geo_key_entry["vertical_units_geo_key"] = "Linear_Yard_Indian";
                          } else {
                            fprintf(file_out, "VerticalUnitsGeoKey: Linear_Yard_Indian\012");
                          }
                          break;
                        case 9014:  // Linear_Fathom
                          if (json_out) {
                            json_geo_key_entry["vertical_units_geo_key"] = "Linear_Fathom";
                          } else {
                            fprintf(file_out, "VerticalUnitsGeoKey: Linear_Fathom\012");
                          }
                          break;
                        case 9015:  // Linear_Mile_International_Nautical
                          if (json_out) {
                            json_geo_key_entry["vertical_units_geo_key"] = "Linear_Mile_International_Nautical";
                          } else {
                            fprintf(file_out, "VerticalUnitsGeoKey: Linear_Mile_International_Nautical\012");
                          }
                          break;
                        default:
                          if (json_out) {
                            char buffer[100];
                            snprintf(buffer, sizeof(buffer), "look-up for %d not implemented", lasreader->header.vlr_geo_key_entries[j].value_offset);
                            json_geo_key_entry["vertical_units_geo_key"] = buffer;
                          } else {
                            fprintf(
                                file_out, "VerticalUnitsGeoKey: look-up for %d not implemented\012",
                                lasreader->header.vlr_geo_key_entries[j].value_offset);
                          }
                      }
                      break;
                    default:
                      if (json_out) {
                        char buffer[100];
                        snprintf(buffer, sizeof(buffer), "key ID %d not implemented", lasreader->header.vlr_geo_key_entries[j].key_id);
                        json_geo_key_entry["warnings"].push_back(buffer);
                      } else {
                        fprintf(file_out, "key ID %d not implemented\012", lasreader->header.vlr_geo_key_entries[j].key_id);
                      }
                  }
                }
                if (json_out) json_vlr_record["geo_key_directory_tag"]["geo_keys"].push_back(json_geo_key_entry);
              }
            } else if (lasheader->vlrs[i].record_id == 34736) {  // GeoDoubleParamsTag
              if (json_out) {
                json_vlr_record["geo_double_params_tag"];
                json_vlr_record["geo_double_params_tag"]["number_of_doubles"] = lasreader->header.vlrs[i].record_length_after_header / 8;
                json_vlr_record["geo_double_params_tag"]["geo_params"];
              } else {
                fprintf(file_out, "    GeoDoubleParamsTag (number of doubles %d)\012", lasreader->header.vlrs[i].record_length_after_header / 8);
                fprintf(file_out, "      ");
              }
              for (int j = 0; j < lasreader->header.vlrs[i].record_length_after_header / 8; j++) {
                if (json_out) {
                  json_vlr_record["geo_double_params_tag"]["geo_params"].push_back(lasheader->vlr_geo_double_params[j]);
                } else {
                  fprintf(file_out, "%lg ", lasheader->vlr_geo_double_params[j]);
                }
              }
              if (!json_out) fprintf(file_out, "\012");
            } else if (lasheader->vlrs[i].record_id == 34737) {  // GeoAsciiParamsTag
              if (json_out) {
                json_vlr_record["geo_ascii_params_tag"];
                json_vlr_record["geo_ascii_params_tag"]["number_of_characters"] = lasreader->header.vlrs[i].record_length_after_header;
                json_vlr_record["geo_ascii_params_tag"]["geo_params"];
              } else {
                fprintf(file_out, "    GeoAsciiParamsTag (number of characters %d)\012", lasreader->header.vlrs[i].record_length_after_header);
                fprintf(file_out, "      ");
              }
              for (int j = 0; j < lasreader->header.vlrs[i].record_length_after_header; j++) {
                if (lasheader->vlr_geo_ascii_params[j] >= ' ') {
                  if (json_out) {
                    json_vlr_record["geo_ascii_params_tag"]["geo_params"].push_back(lasheader->vlr_geo_ascii_params[j]);
                  } else {
                    fprintf(file_out, "%c", lasheader->vlr_geo_ascii_params[j]);
                  }
                } else {
                  if (!json_out) fprintf(file_out, " ");
                }
              }
              if (!json_out) fprintf(file_out, "\012");
            } else if (lasheader->vlrs[i].record_id == 2111) {  // WKT OGC MATH TRANSFORM
              if (json_out) {
                json_vlr_record["wkt_ogc_math_transform"] = reinterpret_cast<char*>(lasreader->header.vlrs[i].data);
              } else {
                fprintf(file_out, "    WKT OGC MATH TRANSFORM:\012");
                fprintf(file_out, "    %s\012", lasreader->header.vlrs[i].data);
              }
            } else if (lasheader->vlrs[i].record_id == 2112) {  // WKT OGC COORDINATE SYSTEM
              if (json_out) {
                json_vlr_record["wkt_ogc_coordinate_system"] = reinterpret_cast<char*>(lasreader->header.vlrs[i].data);
              } else {
                fprintf(file_out, "    WKT OGC COORDINATE SYSTEM:\012");
                fprintf(file_out, "    %s\012", lasreader->header.vlrs[i].data);
              }
            }
          } else if ((strcmp(lasheader->vlrs[i].user_id, "LASF_Spec") == 0) && (lasheader->vlrs[i].data != 0)) {
            if (lasheader->vlrs[i].record_id == 0) {  // ClassificationLookup
              LASvlr_classification* vlr_classification = (LASvlr_classification*)lasheader->vlrs[i].data;
              int num = lasheader->vlrs[i].record_length_after_header / sizeof(LASvlr_classification);
              if (json_out) json_vlr_record["classification"];

              for (int j = 0; j < num; j++) {
                if (json_out) {
                  JsonObject json_classification;
                  json_classification["class_number"] = vlr_classification[j].class_number;
                  json_classification["class_description"] = vlr_classification[j].description;
                  json_vlr_record["classification"].push_back(json_classification);
                } else {
                  fprintf(file_out, "    %d %.15s", vlr_classification[j].class_number, vlr_classification[j].description);
                }
              }
              if (num && !json_out) fprintf(file_out, "\012");
            } else if (lasheader->vlrs[i].record_id == 2) {  // Histogram
            } else if (lasheader->vlrs[i].record_id == 3) {  // TextAreaDescription
              if (json_out) {
                json_vlr_record["text_area_description"];
              } else {
                fprintf(file_out, "    ");
              }
              for (int j = 0; j < lasheader->vlrs[i].record_length_after_header; j++) {
                if (lasheader->vlrs[i].data[j] != '\0') {
                  if (json_out) {
                    json_vlr_record["text_area_description"].push_back(reinterpret_cast<char*>(lasreader->header.vlrs[i].data));
                  } else {
                    fprintf(file_out, "%c", lasheader->vlrs[i].data[j]);
                  }
                } else if (!json_out) {
                  fprintf(file_out, " ");
                }
              }
              if (!json_out) fprintf(file_out, "\012");
            } else if (lasheader->vlrs[i].record_id == 4) {  // ExtraBytes
              const char* name_table[10] = {"unsigned char",      "char",      "unsigned short", "short", "unsigned long", "long",
                                            "unsigned long long", "long long", "float",          "double"};
              if (json_out) {
                json_vlr_record["extra_byte_descriptions"];
              } else {
                fprintf(file_out, "    Extra Byte Descriptions\012");
              }
              for (int j = 0; j < lasheader->vlrs[i].record_length_after_header; j += 192) {
                if (lasheader->vlrs[i].data[j + 2]) {
                  int type = ((I32)(lasheader->vlrs[i].data[j + 2]) - 1) % 10;
                  int dim = ((I32)(lasheader->vlrs[i].data[j + 2]) - 1) / 10 + 1;
                  if (file_out) {
                    JsonObject json_extra_byte;
                    if (json_out) {
                      json_extra_byte["data_type"] = (I32)(lasheader->vlrs[i].data[j + 2]);
                      json_extra_byte["type"] = name_table[type];
                      json_extra_byte["name"] = (char*)(lasheader->vlrs[i].data + j + 4);
                      json_extra_byte["description"] = (char*)(lasheader->vlrs[i].data + j + 160);
                      // json_vlr_record["extra_byte_descriptions"].push_back(json_extra_byte);
                    } else {
                      fprintf(
                          file_out, "      data type: %d (%s), name \"%s\", description: \"%s\"", (I32)(lasheader->vlrs[i].data[j + 2]),
                          name_table[type], (char*)(lasheader->vlrs[i].data + j + 4), (char*)(lasheader->vlrs[i].data + j + 160));
                    }
                    if (lasheader->vlrs[i].data[j + 3] & 0x02) {  // if min is set
                      if (json_out) {
                        json_extra_byte["min"];
                      } else {
                        fprintf(file_out, ", min:");
                      }
                      for (int k = 0; k < dim; k++) {
                        if (type < 8) {
                          if (json_out) {
                            json_extra_byte["min"].push_back(((I64*)(lasheader->vlrs[i].data + j + 64))[k]);
                          } else {
                            fprintf(file_out, ", %lld", ((I64*)(lasheader->vlrs[i].data + j + 64))[k]);
                          }
                        } else {
                          if (json_out) {
                            json_extra_byte["min"].push_back(((F64*)(lasheader->vlrs[i].data + j + 64))[k]);
                          } else {
                            fprintf(file_out, " %g", ((F64*)(lasheader->vlrs[i].data + j + 64))[k]);
                          }
                        }
                      }
                    }
                    if (lasheader->vlrs[i].data[j + 3] & 0x04) {  // if max is set
                      if (json_out) {
                        json_extra_byte["max"];
                      } else {
                        fprintf(file_out, ", max:");
                      }
                      for (int k = 0; k < dim; k++) {
                        if (type < 8) {
                          if (json_out) {
                            json_extra_byte["min"].push_back(((I64*)(lasheader->vlrs[i].data + j + 88))[k]);
                          } else {
                            fprintf(file_out, ", %lld", ((I64*)(lasheader->vlrs[i].data + j + 88))[k]);
                          }
                        } else {
                          if (json_out) {
                            json_extra_byte["min"].push_back(((F64*)(lasheader->vlrs[i].data + j + 88))[k]);
                          } else {
                            fprintf(file_out, " %g", ((F64*)(lasheader->vlrs[i].data + j + 88))[k]);
                          }
                        }
                      }
                    }
                    if (lasheader->vlrs[i].data[j + 3] & 0x08) {  // if scale is set
                      if (json_out) {
                        json_extra_byte["scale"];
                      } else {
                        fprintf(file_out, ", scale:");
                      }
                      for (int k = 0; k < dim; k++) {
                        if (json_out) {
                          json_extra_byte["scale"].push_back(((F64*)(lasheader->vlrs[i].data + j + 112))[k]);
                        } else {
                          fprintf(file_out, " %g", ((F64*)(lasheader->vlrs[i].data + j + 112))[k]);
                        }
                      }
                    } else {
                      if (json_out) {
                        json_extra_byte["scale"] = nullptr;
                      } else {
                        fprintf(file_out, ", scale: 1 (not set)");
                      }
                    }
                    if (lasheader->vlrs[i].data[j + 3] & 0x10) {  // if offset is set
                      if (json_out) {
                        json_extra_byte["offset"];
                      } else {
                        fprintf(file_out, ", offset:");
                      }
                      for (int k = 0; k < dim; k++) {
                        if (json_out) {
                          json_extra_byte["offset"].push_back(((F64*)(lasheader->vlrs[i].data + j + 136))[k]);
                        } else {
                          fprintf(file_out, " %g", ((F64*)(lasheader->vlrs[i].data + j + 136))[k]);
                        }
                      }
                    } else {
                      if (json_out) {
                        json_extra_byte["offset"] = nullptr;
                      } else {
                        fprintf(file_out, ", offset: 0 (not set)");
                      }
                    }
                    if (json_out) {
                      json_vlr_record["extra_byte_descriptions"].push_back(json_extra_byte);
                    } else {
                      fprintf(file_out, "\012");
                    }
                  }
                } else {
                  if (json_out) {
                    JsonObject json_extra_byte;
                    json_extra_byte["data_type"] = 0;
                    json_extra_byte["type"] = "untyped bytes";
                    json_extra_byte["size"] = lasheader->vlrs[i].data[j + 3];
                    json_vlr_record["extra_byte_descriptions"].push_back(json_extra_byte);
                    ;
                  } else {
                    fprintf(file_out, "      data type: 0 (untyped bytes), size: %d\012", lasheader->vlrs[i].data[j + 3]);
                  }
                }
              }
            } else if ((lasheader->vlrs[i].record_id >= 100) && (lasheader->vlrs[i].record_id < 355)) {  // WavePacketDescriptor
              LASvlr_wave_packet_descr* vlr_wave_packet_descr = (LASvlr_wave_packet_descr*)lasheader->vlrs[i].data;
              if (json_out) {
                json_vlr_record["wave_packet_descriptor"];
                json_vlr_record["wave_packet_descriptor"]["index"] = lasheader->vlrs[i].record_id - 99;
                json_vlr_record["wave_packet_descriptor"]["bits_per_sample"] = vlr_wave_packet_descr->getBitsPerSample();
                json_vlr_record["wave_packet_descriptor"]["compression"] = vlr_wave_packet_descr->getCompressionType();
                json_vlr_record["wave_packet_descriptor"]["samples"] = vlr_wave_packet_descr->getNumberOfSamples();
                json_vlr_record["wave_packet_descriptor"]["temporal"] = vlr_wave_packet_descr->getTemporalSpacing();
                json_vlr_record["wave_packet_descriptor"]["gain"] = vlr_wave_packet_descr->getDigitizerGain();
                json_vlr_record["wave_packet_descriptor"]["offset"] = vlr_wave_packet_descr->getDigitizerOffset();
              } else {
                fprintf(
                    file_out, "  index %d bits/sample %d compression %d samples %u temporal %u gain %lg, offset %lg\012",
                    lasheader->vlrs[i].record_id - 99, vlr_wave_packet_descr->getBitsPerSample(), vlr_wave_packet_descr->getCompressionType(),
                    vlr_wave_packet_descr->getNumberOfSamples(), vlr_wave_packet_descr->getTemporalSpacing(),
                    vlr_wave_packet_descr->getDigitizerGain(), vlr_wave_packet_descr->getDigitizerOffset());
              }
            }
          } else if ((strcmp(lasheader->vlrs[i].user_id, "Raster LAZ") == 0) && (lasheader->vlrs[i].record_id == 7113)) {
            LASvlrRasterLAZ vlrRasterLAZ;
            if (json_out) json_vlr_record["raster_laz"];

            if (vlrRasterLAZ.set_payload(lasheader->vlrs[i].data, lasheader->vlrs[i].record_length_after_header)) {
              if (json_out) {
                json_vlr_record["raster_laz"]["ncols"] = vlrRasterLAZ.ncols;
                json_vlr_record["raster_laz"]["nrows"] = vlrRasterLAZ.nrows;
                json_vlr_record["raster_laz"]["llx"] = round_to_decimals(vlrRasterLAZ.llx, 10);
                json_vlr_record["raster_laz"]["lly"] = round_to_decimals(vlrRasterLAZ.lly, 10);
                json_vlr_record["raster_laz"]["stepx"] = vlrRasterLAZ.stepx;
                json_vlr_record["raster_laz"]["stepy"] = vlrRasterLAZ.stepy;
              } else {
                fprintf(file_out, "    ncols %6d\012", vlrRasterLAZ.ncols);
                fprintf(file_out, "    nrows %6d\012", vlrRasterLAZ.nrows);
                fprintf(file_out, "    llx   %.10g\012", vlrRasterLAZ.llx);
                fprintf(file_out, "    lly   %.10g\012", vlrRasterLAZ.lly);
                fprintf(file_out, "    stepx    %g\012", vlrRasterLAZ.stepx);
                fprintf(file_out, "    stepy    %g\012", vlrRasterLAZ.stepy);
              }
              if (vlrRasterLAZ.sigmaxy) {
                if (json_out) {
                  json_vlr_record["raster_laz"]["sigmaxy"] = vlrRasterLAZ.sigmaxy;
                } else {
                  fprintf(file_out, "    sigmaxy %g\012", vlrRasterLAZ.sigmaxy);
                }
              } else {
                if (json_out) {
                  json_vlr_record["raster_laz"]["sigmaxy"] = nullptr;
                } else {
                  fprintf(file_out, "    sigmaxy <not set>\012");
                }
              }
            } else {
              if (json_out) {
                json_vlr_record["warnings"].push_back("corrupt RasterLAZ VLR");
              } else {
                fprintf(file_out, "WARNING: corrupt RasterLAZ VLR\n");
              }
            }
          } else if ((strcmp(lasheader->vlrs[i].user_id, "copc") == 0) && (lasheader->vlrs[i].record_id == 1)) {
            if (json_out) json_vlr_record["copc"];
            LASvlr_copc_info* info = (LASvlr_copc_info*)lasheader->vlrs[i].data;
            if (json_out) {
              JsonObject json_copc;
              lidardouble2string(printstring, info->center_x, lasheader->x_scale_factor);
              json_copc["center"]["x"] = parseFormattedDouble(printstring);
              lidardouble2string(printstring, info->center_y, lasheader->y_scale_factor);
              json_copc["center"]["y"] = parseFormattedDouble(printstring);
              lidardouble2string(printstring, info->center_z, lasheader->z_scale_factor);
              json_copc["center"]["z"] = parseFormattedDouble(printstring);
              json_copc["root_node_halfsize"] = info->halfsize;
              json_copc["root_node_point_spacing"] = info->spacing;
              json_copc["gpstime"]["min"] = info->gpstime_minimum;
              json_copc["gpstime"]["max"] = info->gpstime_maximum;
              json_copc["root_hierarchy"]["offset"] = info->root_hier_offset;
              json_copc["root_hierarchy"]["size"] = info->root_hier_size;
              json_vlr_record["copc"] = json_copc;
            } else {
              fprintf(file_out, "    center x y z: ");
              lidardouble2string(printstring, info->center_x, lasheader->x_scale_factor);
              fprintf(file_out, "%s ", printstring);
              lidardouble2string(printstring, info->center_y, lasheader->y_scale_factor);
              fprintf(file_out, "%s ", printstring);
              lidardouble2string(printstring, info->center_z, lasheader->z_scale_factor);
              fprintf(file_out, "%s\012", printstring);
              fprintf(file_out, "    root node halfsize: %.3lf\012", info->halfsize);
              fprintf(file_out, "    root node point spacing: %.3lf\012", info->spacing);
              fprintf(file_out, "    gpstime min/max: %.2lf/%.2lf\012", info->gpstime_minimum, info->gpstime_maximum);
#ifdef _WIN32
              fprintf(file_out, "    root hierarchy offset/size: %I64u/%I64u\012", info->root_hier_offset, info->root_hier_size);
#else
              fprintf(file_out, "    root hierarchy offset/size: %llu/%llu\012", info->root_hier_offset, info->root_hier_size);
#endif
            }
          }
          if (json_out) json_sub_main["las_variable_length_records"].push_back(json_vlr_record);
        }
      }

      if (file_out && !no_variable_header) {
        JsonObject json_evlr_record;

        for (int i = 0; i < (int)lasheader->number_of_extended_variable_length_records; i++) {
          if (json_out) {
            json_evlr_record["record_number"] = i + 1;
            json_evlr_record["total_records"] = (int)lasheader->number_of_extended_variable_length_records;
            json_evlr_record["reserved"] = lasreader->header.evlrs[i].reserved;
            json_evlr_record["user_id"] = lasreader->header.evlrs[i].user_id;
            json_evlr_record["record_id"] = lasreader->header.evlrs[i].record_id;
            json_evlr_record["record_length_after_header"] = lasreader->header.evlrs[i].record_length_after_header;
            json_evlr_record["description"] = lasreader->header.evlrs[i].description;
          } else {
            fprintf(
                file_out, "extended variable length header record %d of %d:\012", i + 1, (int)lasheader->number_of_extended_variable_length_records);
            fprintf(file_out, "  reserved             %d\012", lasreader->header.evlrs[i].reserved);
            fprintf(file_out, "  user ID              '%.16s'\012", lasreader->header.evlrs[i].user_id);
            fprintf(file_out, "  record ID            %d\012", lasreader->header.evlrs[i].record_id);
            fprintf(file_out, "  length after header  %lld\012", lasreader->header.evlrs[i].record_length_after_header);
            fprintf(file_out, "  description          '%.32s'\012", lasreader->header.evlrs[i].description);
          }

          if (strcmp(lasheader->evlrs[i].user_id, "LASF_Projection") == 0) {
            if (lasheader->evlrs[i].record_id == 2111) {  // OGC MATH TRANSFORM WKT
              if (json_out) {
                json_evlr_record["wkt_ogc_math_transform"] = reinterpret_cast<char*>(lasreader->header.evlrs[i].data);
              } else {
                fprintf(file_out, "    OGC MATH TRANSFORM WKT:\012");
                fprintf(file_out, "    %s\012", lasreader->header.evlrs[i].data);
              }
            } else if (lasheader->evlrs[i].record_id == 2112) {  // OGC COORDINATE SYSTEM WKT
              if (json_out) {
                json_evlr_record["wkt_ogc_coordinate_system"] = reinterpret_cast<char*>(lasreader->header.evlrs[i].data);
              } else {
                fprintf(file_out, "    OGC COORDINATE SYSTEM WKT:\012");
                fprintf(file_out, "    %s\012", lasreader->header.evlrs[i].data);
              }
            }
          } else if (strcmp(lasheader->evlrs[i].user_id, "copc") == 0) {
            if (lasheader->evlrs[i].record_id == 1000) {  // COPC EPT hierachy
              if (lasheader->vlr_copc_entries) {
                I32 max_octree_level = 0;
                for (U32 j = 0; j < lasheader->number_of_copc_entries; j++) {
                  if (lasheader->vlr_copc_entries[j].key.depth > max_octree_level) max_octree_level = lasheader->vlr_copc_entries[j].key.depth;
                }
                max_octree_level++;

                if (json_out) {
                  json_evlr_record["copc"]["octree_level_number"] = max_octree_level;
                } else {
                  fprintf(file_out, "    Octree with %d levels\012", max_octree_level);
                }

                U64 total = 0;
                U64* point_count = (U64*)calloc(max_octree_level, sizeof(U64));
                U32* voxel_count = (U32*)calloc(max_octree_level, sizeof(U32));

                for (U32 j = 0; j < lasheader->number_of_copc_entries; j++) {
                  total += lasheader->vlr_copc_entries[j].point_count;
                  point_count[lasheader->vlr_copc_entries[j].key.depth] += lasheader->vlr_copc_entries[j].point_count;
                  voxel_count[lasheader->vlr_copc_entries[j].key.depth]++;
                }

                for (I32 j = 0; j < max_octree_level; j++) {
                  if (json_out) {
                    JsonObject json_copc;
                    json_copc["level"] = j;
                    json_copc["points"] = point_count[j];
                    json_copc["voxels"] = voxel_count[j];
                    json_evlr_record["copc"]["octree_levels"].push_back(json_copc);
                  } else {
#ifdef _WIN32
                    fprintf(file_out, "    Level %d : %I64u points in %u voxels\012", j, point_count[j], voxel_count[j]);
#else
                    fprintf(file_out, "    Level %d : %llu points in %u voxels\012", j, point_count[j], voxel_count[j]);
#endif
                  }
                }
                free(point_count);
                free(voxel_count);
              } else {
                if (json_out) {
                  json_evlr_record["error"] = "invalid COPC file, EPT hierachy not parsed";
                } else {
                  fprintf(file_out, "  ERROR: invalid COPC file, EPT hierachy not parsed.\n");
                }
              }
            }
          }
          if (json_out) json_sub_main["las_extended_variable_length_records"].push_back(json_evlr_record);
        }
      }

      if (file_out && !no_variable_header) {
        const LASindex* index = lasreader->get_index();
        if (index) {
          if (json_out) {
            json_sub_main["spatial_indexing_lax_file"] = true;
          } else {
            fprintf(file_out, "has spatial indexing LAX file\012");  // index->start, index->end, index->full, index->total, index->cells);
          }
        } else if (json_out) {
          json_sub_main["spatial_indexing_lax_file"] = false;
        }
      }

      if (file_out && !no_header) {
        if (lasheader->user_data_after_header_size) {
          if (json_out) {
            json_sub_main["user_defined_bytes_after_header"] = lasheader->user_data_after_header_size;
          } else {
            fprintf(file_out, "the header is followed by %u user-defined bytes\012", lasheader->user_data_after_header_size);
          }
        }

        if (lasheader->laszip) {
          if (json_out) {
            char buffer[100];
            snprintf(
                buffer, sizeof(buffer), "%d.%dr%d c%d", lasheader->laszip->version_major, lasheader->laszip->version_minor,
                lasheader->laszip->version_revision, lasheader->laszip->compressor);
            json_sub_main["laszip_compression"]["version"] = buffer;
          } else {
            fprintf(
                file_out, "LASzip compression (version %d.%dr%d c%d", lasheader->laszip->version_major, lasheader->laszip->version_minor,
                lasheader->laszip->version_revision, lasheader->laszip->compressor);
          }
          if ((lasheader->laszip->compressor == LASZIP_COMPRESSOR_CHUNKED) || (lasheader->laszip->compressor == LASZIP_COMPRESSOR_LAYERED_CHUNKED))
            if (json_out) {
              json_sub_main["laszip_compression"]["chunk_size"] = lasheader->laszip->chunk_size;
            } else {
              fprintf(file_out, " %d):", lasheader->laszip->chunk_size);
            }
          else if (!json_out) {
            fprintf(file_out, "):");
          }
          for (i = 0; i < (int)lasheader->laszip->num_items; i++) {
            if (json_out) {
              JsonObject json_compression_name;
              json_compression_name["name"] = lasheader->laszip->items[i].get_name();
              json_compression_name["version"] = lasheader->laszip->items[i].version;
              json_sub_main["laszip_compression"]["data_structures"].push_back(json_compression_name);
            } else {
              fprintf(file_out, " %s %d", lasheader->laszip->items[i].get_name(), lasheader->laszip->items[i].version);
            }
          }
          if (!json_out) fprintf(file_out, "\012");
        }
        if (lasheader->vlr_lastiling) {
          LASquadtree lasquadtree;
          lasquadtree.subtiling_setup(
              lasheader->vlr_lastiling->min_x, lasheader->vlr_lastiling->max_x, lasheader->vlr_lastiling->min_y, lasheader->vlr_lastiling->max_y,
              lasheader->vlr_lastiling->level, lasheader->vlr_lastiling->level_index, 0);
          F32 min[2], max[2];
          lasquadtree.get_cell_bounding_box(lasheader->vlr_lastiling->level_index, min, max);
          F32 buffer = 0.0f;

          if (lasheader->vlr_lastiling->buffer) {
            buffer = (F32)(min[0] - lasheader->min_x);
            if ((F32)(min[1] - lasheader->min_y) > buffer) buffer = (F32)(min[1] - lasheader->min_y);
            if ((F32)(lasheader->max_x - max[0]) > buffer) buffer = (F32)(lasheader->max_x - max[0]);
            if ((F32)(lasheader->max_y - max[1]) > buffer) buffer = (F32)(lasheader->max_y - max[1]);
          }
          if (json_out) {
            json_sub_main["lastiling"]["index"] = lasheader->vlr_lastiling->level_index;
            json_sub_main["lastiling"]["level"] = lasheader->vlr_lastiling->level;
            json_sub_main["lastiling"]["implicit_levels"] = static_cast<U32>(lasheader->vlr_lastiling->implicit_levels);
            JsonObject json_bbox;
            json_bbox["min_x"] = round_to_decimals(lasheader->vlr_lastiling->min_x, 10);
            json_bbox["min_y"] = round_to_decimals(lasheader->vlr_lastiling->min_y, 10);
            json_bbox["max_x"] = round_to_decimals(lasheader->vlr_lastiling->max_x, 10);
            json_bbox["max_y"] = round_to_decimals(lasheader->vlr_lastiling->max_y, 10);
            json_sub_main["lastiling"]["bbox"] = json_bbox;

            if (lasheader->vlr_lastiling->buffer) {
              json_sub_main["lastiling"]["buffer"] = true;
            } else {
              json_sub_main["lastiling"]["buffer"] = false;
            }

            if (lasheader->vlr_lastiling->reversible) {
              json_sub_main["lastiling"]["reversible"] = true;
            } else {
              json_sub_main["lastiling"]["reversible"] = false;
            }

            JsonObject json_size;
            json_size["width"] = max[0] - min[0];
            json_size["height"] = max[1] - min[1];
            json_sub_main["lastiling"]["size"] = json_size;
            json_sub_main["lastiling"]["buffer_size"] = buffer;
          } else {
            fprintf(
                file_out, "LAStiling (idx %d, lvl %d, sub %d, bbox %.10g %.10g %.10g %.10g%s%s) (size %g x %g, buffer %g)\n",
                lasheader->vlr_lastiling->level_index, lasheader->vlr_lastiling->level, lasheader->vlr_lastiling->implicit_levels,
                lasheader->vlr_lastiling->min_x, lasheader->vlr_lastiling->min_y, lasheader->vlr_lastiling->max_x, lasheader->vlr_lastiling->max_y,
                (lasheader->vlr_lastiling->buffer ? ", buffer" : ""), (lasheader->vlr_lastiling->reversible ? ", reversible" : ""), max[0] - min[0],
                max[1] - min[1], buffer);
          }
        }
        if (lasheader->vlr_lasoriginal) {
          if (json_out) {
            json_sub_main["lasoriginal"]["npoints"] = (U32)lasheader->vlr_lasoriginal->number_of_point_records;
            JsonObject json_bbox;
            json_bbox["min_x"] = round_to_decimals(lasheader->vlr_lasoriginal->min_x, 10);
            json_bbox["min_y"] = round_to_decimals(lasheader->vlr_lasoriginal->min_y, 10);
            json_bbox["min_z"] = round_to_decimals(lasheader->vlr_lasoriginal->min_z, 10);
            json_bbox["max_x"] = round_to_decimals(lasheader->vlr_lasoriginal->max_x, 10);
            json_bbox["max_y"] = round_to_decimals(lasheader->vlr_lasoriginal->max_y, 10);
            json_bbox["max_z"] = round_to_decimals(lasheader->vlr_lasoriginal->max_z, 10);
            json_sub_main["lasoriginal"]["bbox"] = json_bbox;
          } else {
            fprintf(
                file_out, "LASoriginal (npoints %u, bbox %.10g %.10g %.10g %.10g %.10g %.10g)\n",
                (U32)lasheader->vlr_lasoriginal->number_of_point_records, lasheader->vlr_lasoriginal->min_x, lasheader->vlr_lasoriginal->min_y,
                lasheader->vlr_lasoriginal->min_z, lasheader->vlr_lasoriginal->max_x, lasheader->vlr_lasoriginal->max_y,
                lasheader->vlr_lasoriginal->max_z);
          }
        }
      }
      // loop over the points
      F64 enlarged_min_x = lasreader->header.min_x - 0.25 * lasreader->header.x_scale_factor;
      F64 enlarged_max_x = lasreader->header.max_x + 0.25 * lasreader->header.x_scale_factor;
      F64 enlarged_min_y = lasreader->header.min_y - 0.25 * lasreader->header.y_scale_factor;
      F64 enlarged_max_y = lasreader->header.max_y + 0.25 * lasreader->header.y_scale_factor;
      F64 enlarged_min_z = lasreader->header.min_z - 0.25 * lasreader->header.z_scale_factor;
      F64 enlarged_max_z = lasreader->header.max_z + 0.25 * lasreader->header.z_scale_factor;
      LASsummary lassummary;

      if (check_points) {
        I64 num_first_returns = 0;
        I64 num_intermediate_returns = 0;
        I64 num_last_returns = 0;
        I64 num_single_returns = 0;
        I64 num_all_returns = 0;
        I64 outside_bounding_box = 0;
        LASoccupancyGrid* lasoccupancygrid = 0;

        if (compute_density) {
          lasoccupancygrid = new LASoccupancyGrid(horizontal_units > 9001 ? 6.0f : 2.0f);
        }

        if (file_out && !no_min_max && !json_out) fprintf(file_out, "reporting minimum and maximum for all LAS point record entries ...\012");

        // maybe seek to start position

        if (subsequence_start) lasreader->seek(subsequence_start);

        while (lasreader->read_point()) {
          if (lasreader->p_count > subsequence_stop) break;

          if (check_outside) {
            if (!lasreader->point.inside_bounding_box(
                    enlarged_min_x, enlarged_min_y, enlarged_min_z, enlarged_max_x, enlarged_max_y, enlarged_max_z)) {
              outside_bounding_box++;
              if (file_out && report_outside) {
                if (json_out) {
                  JsonObject json_outside_box;
                  json_outside_box["count"] = (U32)(lasreader->p_count - 1);
                  json_outside_box["get_gps_time"] = lasreader->point.get_gps_time();
                  json_outside_box["x"] = lasreader->point.get_x();
                  json_outside_box["y"] = lasreader->point.get_y();
                  json_outside_box["z"] = lasreader->point.get_z();
                  json_outside_box["intensity"] = lasreader->point.get_intensity();
                  json_outside_box["return_number"] = lasreader->point.get_return_number();
                  json_outside_box["number_of_returns"] = lasreader->point.get_number_of_returns();
                  json_outside_box["scan_direction_flag"] = lasreader->point.get_scan_direction_flag();
                  json_outside_box["edge_flight_line"] = lasreader->point.get_edge_of_flight_line();
                  json_outside_box["classification"] = lasreader->point.get_classification();
                  json_outside_box["scan_angle_rank"] = lasreader->point.get_scan_angle_rank();
                  json_outside_box["user_data"] = lasreader->point.get_user_data();
                  json_outside_box["point_source_id"] = lasreader->point.get_point_source_ID();
                  json_sub_main["points_outside_boundig_box"].push_back(json_outside_box);
                } else {
                  fprintf(
                      file_out, "%u t %g x %g y %g z %g i %d (%d of %d) d %d e %d c %d s %d %u p %d \012", (U32)(lasreader->p_count - 1),
                      lasreader->point.get_gps_time(), lasreader->point.get_x(), lasreader->point.get_y(), lasreader->point.get_z(),
                      lasreader->point.get_intensity(), lasreader->point.get_return_number(), lasreader->point.get_number_of_returns(),
                      lasreader->point.get_scan_direction_flag(), lasreader->point.get_edge_of_flight_line(), lasreader->point.get_classification(),
                      lasreader->point.get_scan_angle_rank(), lasreader->point.get_user_data(), lasreader->point.get_point_source_ID());
                }
              }
            }
          }

          lassummary.add(&lasreader->point);

          if (lasoccupancygrid) {
            lasoccupancygrid->add(&lasreader->point);
          }

          if (lasreader->point.is_first()) {
            num_first_returns++;
          }
          if (lasreader->point.is_intermediate()) {
            num_intermediate_returns++;
          }
          if (lasreader->point.is_last()) {
            num_last_returns++;
          }
          if (lasreader->point.is_single()) {
            num_single_returns++;
          }
          num_all_returns++;

          if (lashistogram.active()) {
            lashistogram.add(&lasreader->point);
          }

          if (file_out && progress && (lasreader->p_count % progress) == 0) {
            if (json_out) {
              if (lasreader->p_count > 0) json_sub_main["processed_points"] = lasreader->p_count;
            } else {
              fprintf(file_out, " ... processed %lld points ...\012", lasreader->p_count);
            }
          }
        }
        if (file_out && !no_min_max) {
          JsonObject json_las_point_report;
          if (json_out) {
            json_las_point_report["x"]["min"] = lassummary.min.get_X();
            json_las_point_report["y"]["min"] = lassummary.min.get_Y();
            json_las_point_report["z"]["min"] = lassummary.min.get_Z();
            json_las_point_report["x"]["max"] = lassummary.max.get_X();
            json_las_point_report["y"]["max"] = lassummary.max.get_Y();
            json_las_point_report["z"]["max"] = lassummary.max.get_Z();
            json_las_point_report["intensity"]["min"] = lassummary.min.intensity;
            json_las_point_report["intensity"]["max"] = lassummary.max.intensity;
            json_las_point_report["return_number"]["min"] = static_cast<int>(lassummary.min.return_number);
            json_las_point_report["return_number"]["max"] = static_cast<int>(lassummary.max.return_number);
            json_las_point_report["number_of_returns"]["min"] = static_cast<int>(lassummary.min.number_of_returns);
            json_las_point_report["number_of_returns"]["max"] = static_cast<int>(lassummary.max.number_of_returns);
            json_las_point_report["edge_of_flight_line"]["min"] = static_cast<int>(lassummary.min.edge_of_flight_line);
            json_las_point_report["edge_of_flight_line"]["max"] = static_cast<int>(lassummary.max.edge_of_flight_line);
            json_las_point_report["scan_direction_flag"]["min"] = static_cast<int>(lassummary.min.scan_direction_flag);
            json_las_point_report["scan_direction_flag"]["max"] = static_cast<int>(lassummary.max.scan_direction_flag);
            json_las_point_report["classification"]["min"] = static_cast<int>(lassummary.min.classification);
            json_las_point_report["classification"]["max"] = static_cast<int>(lassummary.max.classification);
            json_las_point_report["scan_angle_rank"]["min"] = lassummary.min.scan_angle_rank;
            json_las_point_report["scan_angle_rank"]["max"] = lassummary.max.scan_angle_rank;
            json_las_point_report["user_data"]["min"] = lassummary.min.user_data;
            json_las_point_report["user_data"]["max"] = lassummary.max.user_data;
            json_las_point_report["point_source_id"]["min"] = lassummary.min.point_source_ID;
            json_las_point_report["point_source_id"]["max"] = lassummary.max.point_source_ID;
          } else {
            fprintf(file_out, "  X          %10d %10d\012", lassummary.min.get_X(), lassummary.max.get_X());
            fprintf(file_out, "  Y          %10d %10d\012", lassummary.min.get_Y(), lassummary.max.get_Y());
            fprintf(file_out, "  Z          %10d %10d\012", lassummary.min.get_Z(), lassummary.max.get_Z());
            fprintf(file_out, "  intensity  %10d %10d\012", lassummary.min.intensity, lassummary.max.intensity);
            fprintf(file_out, "  return_number       %d %10d\012", lassummary.min.return_number, lassummary.max.return_number);
            fprintf(file_out, "  number_of_returns   %d %10d\012", lassummary.min.number_of_returns, lassummary.max.number_of_returns);
            fprintf(file_out, "  edge_of_flight_line %d %10d\012", lassummary.min.edge_of_flight_line, lassummary.max.edge_of_flight_line);
            fprintf(file_out, "  scan_direction_flag %d %10d\012", lassummary.min.scan_direction_flag, lassummary.max.scan_direction_flag);
            fprintf(file_out, "  classification  %5d %10d\012", lassummary.min.classification, lassummary.max.classification);
            fprintf(file_out, "  scan_angle_rank %5d %10d\012", lassummary.min.scan_angle_rank, lassummary.max.scan_angle_rank);
            fprintf(file_out, "  user_data       %5d %10d\012", lassummary.min.user_data, lassummary.max.user_data);
            fprintf(file_out, "  point_source_ID %5d %10d\012", lassummary.min.point_source_ID, lassummary.max.point_source_ID);
          }
          if (lasreader->point.have_gps_time) {
            if (json_out) {
              json_las_point_report["gps_time"]["min"] = lassummary.min.gps_time;
              json_las_point_report["gps_time"]["max"] = lassummary.max.gps_time;
            } else {
              fprintf(file_out, "  gps_time %f %f\012", lassummary.min.gps_time, lassummary.max.gps_time);
            }
            if ((lasreader->header.global_encoding & 1) == 0) {
              if (!no_warnings && (lassummary.min.gps_time < 0.0 || lassummary.max.gps_time > 604800.0)) {
                if (json_out) {
                  json_las_point_report["warnings"].push_back("range violates GPS week time specified by global encoding bit 0");
                } else {
                  fprintf(file_out, "WARNING: range violates GPS week time specified by global encoding bit 0\012");
                }
              }
            } else if (gps_week) {
              I32 week_min = (I32)(lassummary.min.gps_time / 604800.0 + 1653.4391534391534391534391534392);
              I32 week_max = (I32)(lassummary.max.gps_time / 604800.0 + 1653.4391534391534391534391534392);
              I32 secs_min = week_min * 604800 - 1000000000;
              I32 secs_max = week_max * 604800 - 1000000000;
              if (json_out) {
                json_las_point_report["gps_week"]["min"] = week_min;
                json_las_point_report["gps_week"]["max"] = week_max;
                json_las_point_report["gps_secs_of_week"]["min"] = (lassummary.min.gps_time - secs_min);
                json_las_point_report["gps_secs_of_week"]["max"] = (lassummary.max.gps_time - secs_max);
              } else {
                fprintf(file_out, "  gps_week %d %d\012", week_min, week_max);
                fprintf(file_out, "  gps_secs_of_week %f %f\012", (lassummary.min.gps_time - secs_min), (lassummary.max.gps_time - secs_max));
              }
            }
          }
          if (lasreader->point.have_rgb) {
            if (json_out) {
              json_las_point_report["color_r"]["min"] = lassummary.min.rgb[0];
              json_las_point_report["color_r"]["max"] = lassummary.max.rgb[0];
              json_las_point_report["color_g"]["min"] = lassummary.min.rgb[1];
              json_las_point_report["color_g"]["max"] = lassummary.max.rgb[1];
              json_las_point_report["color_b"]["min"] = lassummary.min.rgb[2];
              json_las_point_report["color_b"]["max"] = lassummary.max.rgb[2];
            } else {
              fprintf(file_out, "  Color R %d %d\012", lassummary.min.rgb[0], lassummary.max.rgb[0]);
              fprintf(file_out, "        G %d %d\012", lassummary.min.rgb[1], lassummary.max.rgb[1]);
              fprintf(file_out, "        B %d %d\012", lassummary.min.rgb[2], lassummary.max.rgb[2]);
            }
          }
          if (lasreader->point.have_nir) {
            if (json_out) {
              json_las_point_report["nir"]["min"] = lassummary.min.rgb[3];
              json_las_point_report["nir"]["max"] = lassummary.max.rgb[3];
            } else {
              fprintf(file_out, "      NIR %d %d\012", lassummary.min.rgb[3], lassummary.max.rgb[3]);
            }
          }
          if (lasreader->point.have_wavepacket) {
            if (json_out) {
              json_las_point_report["wavepacket_index"]["min"] = lassummary.min.wavepacket.getIndex();
              json_las_point_report["wavepacket_index"]["max"] = lassummary.max.wavepacket.getIndex();
              json_las_point_report["offset"]["min"] = lassummary.min.wavepacket.getOffset();
              json_las_point_report["offset"]["max"] = lassummary.max.wavepacket.getOffset();
              json_las_point_report["size"]["min"] = lassummary.min.wavepacket.getSize();
              json_las_point_report["size"]["max"] = lassummary.max.wavepacket.getSize();
              json_las_point_report["location"]["min"] = lassummary.min.wavepacket.getLocation();
              json_las_point_report["location"]["max"] = lassummary.max.wavepacket.getLocation();
              json_las_point_report["xt"]["min"] = lassummary.min.wavepacket.getXt();
              json_las_point_report["xt"]["max"] = lassummary.max.wavepacket.getXt();
              json_las_point_report["yt"]["min"] = lassummary.min.wavepacket.getYt();
              json_las_point_report["yt"]["max"] = lassummary.max.wavepacket.getYt();
              json_las_point_report["zt"]["min"] = lassummary.min.wavepacket.getZt();
              json_las_point_report["zt"]["max"] = lassummary.max.wavepacket.getZt();
            } else {
              fprintf(file_out, "  Wavepacket Index    %d %d\012", lassummary.min.wavepacket.getIndex(), lassummary.max.wavepacket.getIndex());
              fprintf(file_out, "             Offset   %lld %lld\012", lassummary.min.wavepacket.getOffset(), lassummary.max.wavepacket.getOffset());
              fprintf(file_out, "             Size     %d %d\012", lassummary.min.wavepacket.getSize(), lassummary.max.wavepacket.getSize());
              fprintf(file_out, "             Location %g %g\012", lassummary.min.wavepacket.getLocation(), lassummary.max.wavepacket.getLocation());
              fprintf(file_out, "             Xt       %g %g\012", lassummary.min.wavepacket.getXt(), lassummary.max.wavepacket.getXt());
              fprintf(file_out, "             Yt       %g %g\012", lassummary.min.wavepacket.getYt(), lassummary.max.wavepacket.getYt());
              fprintf(file_out, "             Zt       %g %g\012", lassummary.min.wavepacket.getZt(), lassummary.max.wavepacket.getZt());
            }
          }
          if (lasreader->point.extended_point_type) {
            if (json_out) {
              json_las_point_report["extended_return_number"]["min"] = static_cast<int>(lassummary.min.extended_return_number);
              json_las_point_report["extended_return_number"]["max"] = static_cast<int>(lassummary.max.extended_return_number);
              json_las_point_report["extended_number_of_returns"]["min"] = static_cast<int>(lassummary.min.extended_number_of_returns);
              json_las_point_report["extended_number_of_returns"]["max"] = static_cast<int>(lassummary.max.extended_number_of_returns);
              json_las_point_report["extended_classification"]["min"] = lassummary.min.extended_classification;
              json_las_point_report["extended_classification"]["max"] = lassummary.max.extended_classification;
              json_las_point_report["extended_scan_angle"]["min"] = lassummary.min.extended_scan_angle;
              json_las_point_report["extended_scan_angle"]["max"] = lassummary.max.extended_scan_angle;
              json_las_point_report["extended_scanner_channel"]["min"] = static_cast<int>(lassummary.min.extended_scanner_channel);
              json_las_point_report["extended_scanner_channel"]["max"] = static_cast<int>(lassummary.max.extended_scanner_channel);
            } else {
              fprintf(
                  file_out, "  extended_return_number     %6d %6d\012", lassummary.min.extended_return_number, lassummary.max.extended_return_number);
              fprintf(
                  file_out, "  extended_number_of_returns %6d %6d\012", lassummary.min.extended_number_of_returns,
                  lassummary.max.extended_number_of_returns);
              fprintf(
                  file_out, "  extended_classification    %6d %6d\012", lassummary.min.extended_classification,
                  lassummary.max.extended_classification);
              fprintf(file_out, "  extended_scan_angle        %6d %6d\012", lassummary.min.extended_scan_angle, lassummary.max.extended_scan_angle);
              fprintf(
                  file_out, "  extended_scanner_channel   %6d %6d\012", lassummary.min.extended_scanner_channel,
                  lassummary.max.extended_scanner_channel);
            }
          }
          if (lasreader->point.extra_bytes_number && lasreader->point.attributer) {
            lassummary.min.attributer = lasreader->point.attributer;
            lassummary.max.attributer = lasreader->point.attributer;
            I32 a;
            for (a = 0; a < lasreader->point.attributer->number_attributes; a++) {
              if (json_out) {
                JsonObject json_attribute;
                json_attribute["index"] = a;
                json_attribute["min"] = lassummary.min.get_attribute_as_float(a);
                json_attribute["max"] = lassummary.max.get_attribute_as_float(a);
                json_attribute["name"] = lasreader->point.attributer->get_attribute_name(a);
                json_las_point_report["attributes"].push_back(json_attribute);
              } else {
                fprintf(
                    file_out, "  attribute%d %10g %10g  ('%s')\012", a, lassummary.min.get_attribute_as_float(a),
                    lassummary.max.get_attribute_as_float(a), lasreader->point.attributer->get_attribute_name(a));
              }
            }
            lassummary.min.attributer = 0;
            lassummary.max.attributer = 0;
          }
          if (((number_of_point_records == 0) && (lasheader->number_of_point_records > 0)) ||
              ((number_of_points_by_return0 == 0) && (lasheader->number_of_points_by_return[0] > 0))) {
            if (json_out) {
              JsonObject json_point_records;
              if ((number_of_point_records == 0) && (lasheader->number_of_point_records > 0))
                json_point_records["number_of_point_records"] = lasheader->number_of_point_records;
              if ((number_of_points_by_return0 == 0) && (lasheader->number_of_points_by_return[0] > 0)) {
                json_point_records["number_of_points_by_return"].push_back(lasheader->number_of_points_by_return[0]);
                json_point_records["number_of_points_by_return"].push_back(lasheader->number_of_points_by_return[1]);
                json_point_records["number_of_points_by_return"].push_back(lasheader->number_of_points_by_return[2]);
                json_point_records["number_of_points_by_return"].push_back(lasheader->number_of_points_by_return[3]);
                json_point_records["number_of_points_by_return"].push_back(lasheader->number_of_points_by_return[4]);
              }
              lidardouble2string(printstring, lasheader->min_x, lasheader->x_scale_factor);
              json_point_records["x"]["min"] = parseFormattedDouble(printstring);
              lidardouble2string(printstring, lasheader->min_y, lasheader->y_scale_factor);
              json_point_records["y"]["min"] = parseFormattedDouble(printstring);
              lidardouble2string(printstring, lasheader->min_z, lasheader->z_scale_factor);
              json_point_records["z"]["min"] = parseFormattedDouble(printstring);
              lidardouble2string(printstring, lasheader->max_x, lasheader->x_scale_factor);
              json_point_records["x"]["max"] = parseFormattedDouble(printstring);
              lidardouble2string(printstring, lasheader->max_y, lasheader->y_scale_factor);
              json_point_records["y"]["max"] = parseFormattedDouble(printstring);
              lidardouble2string(printstring, lasheader->max_z, lasheader->z_scale_factor);
              json_point_records["z"]["max"] = parseFormattedDouble(printstring);

              json_las_point_report["point_records"] = json_point_records;
            } else {
              fprintf(file_out, "re-reporting LAS header entries populated during read pass:\012");
              if ((number_of_point_records == 0) && (lasheader->number_of_point_records > 0))
                fprintf(file_out, "  number of point records    %u\012", lasheader->number_of_point_records);
              if ((number_of_points_by_return0 == 0) && (lasheader->number_of_points_by_return[0] > 0))
                fprintf(
                    file_out, "  number of points by return %u %u %u %u %u\012", lasheader->number_of_points_by_return[0],
                    lasheader->number_of_points_by_return[1], lasheader->number_of_points_by_return[2], lasheader->number_of_points_by_return[3],
                    lasheader->number_of_points_by_return[4]);
              fprintf(file_out, "  min x y z                  ");
              lidardouble2string(printstring, lasheader->min_x, lasheader->x_scale_factor);
              fprintf(file_out, "%s ", printstring);
              lidardouble2string(printstring, lasheader->min_y, lasheader->y_scale_factor);
              fprintf(file_out, "%s ", printstring);
              lidardouble2string(printstring, lasheader->min_z, lasheader->z_scale_factor);
              fprintf(file_out, "%s\012", printstring);
              fprintf(file_out, "  max x y z                  ");
              lidardouble2string(printstring, lasheader->max_x, lasheader->x_scale_factor);
              fprintf(file_out, "%s ", printstring);
              lidardouble2string(printstring, lasheader->max_y, lasheader->y_scale_factor);
              fprintf(file_out, "%s ", printstring);
              lidardouble2string(printstring, lasheader->max_z, lasheader->z_scale_factor);
              fprintf(file_out, "%s\012", printstring);
            }
          }
          if (json_out && !json_las_point_report.is_null()) json_sub_main["min_max_las_point_report"] = json_las_point_report;
        }

        if (!no_warnings && file_out && outside_bounding_box) {
          if (json_out) {
            char buffer[256];
            snprintf(buffer, sizeof(buffer), "%lld points outside of header bounding box", outside_bounding_box);
            json_sub_main["warnings"].push_back(buffer);
          } else {
            fprintf(file_out, "WARNING: %lld points outside of header bounding box\012", outside_bounding_box);
          }
        }
        if (!no_warnings && file_out && lassummary.has_fluff()) {
          if (json_out) {
            char buffer[256];
            snprintf(
                buffer, sizeof(buffer), "there is coordinate resolution fluff (x10) in %s%s%s\012", (lassummary.has_fluff(0) ? "X" : ""),
                (lassummary.has_fluff(1) ? "Y" : ""), (lassummary.has_fluff(2) ? "Z" : ""));
            json_sub_main["warnings"].push_back(buffer);
          } else {
            fprintf(
                file_out, "WARNING: there is coordinate resolution fluff (x10) in %s%s%s\012", (lassummary.has_fluff(0) ? "X" : ""),
                (lassummary.has_fluff(1) ? "Y" : ""), (lassummary.has_fluff(2) ? "Z" : ""));
          }
          if (lassummary.has_serious_fluff()) {
            if (json_out) {
              char buffer[256];
              snprintf(
                  buffer, sizeof(buffer), "there is serious coordinate resolution fluff (x100) in %s%s%s\012",
                  (lassummary.has_serious_fluff(0) ? "X" : ""), (lassummary.has_serious_fluff(1) ? "Y" : ""),
                  (lassummary.has_serious_fluff(2) ? "Z" : ""));
              json_sub_main["warnings"].push_back(buffer);
            } else {
              fprintf(
                  file_out, "WARNING: there is serious coordinate resolution fluff (x100) in %s%s%s\012",
                  (lassummary.has_serious_fluff(0) ? "X" : ""), (lassummary.has_serious_fluff(1) ? "Y" : ""),
                  (lassummary.has_serious_fluff(2) ? "Z" : ""));
            }
            if (lassummary.has_very_serious_fluff()) {
              if (json_out) {
                char buffer[256];
                snprintf(
                    buffer, sizeof(buffer), "there is very serious coordinate resolution fluff (x1000) in %s%s%s\012",
                    (lassummary.has_very_serious_fluff(0) ? "X" : ""), (lassummary.has_very_serious_fluff(1) ? "Y" : ""),
                    (lassummary.has_very_serious_fluff(2) ? "Z" : ""));
                json_sub_main["warnings"].push_back(buffer);
              } else {
                fprintf(
                    file_out, "WARNING: there is very serious coordinate resolution fluff (x1000) in %s%s%s\012",
                    (lassummary.has_very_serious_fluff(0) ? "X" : ""), (lassummary.has_very_serious_fluff(1) ? "Y" : ""),
                    (lassummary.has_very_serious_fluff(2) ? "Z" : ""));
              }
              if (lassummary.has_extremely_serious_fluff()) {
                if (json_out) {
                  char buffer[256];
                  snprintf(
                      buffer, sizeof(buffer), "there is extremely serious coordinate resolution fluff (x10000) in %s%s%s\012",
                      (lassummary.has_extremely_serious_fluff(0) ? "X" : ""), (lassummary.has_extremely_serious_fluff(1) ? "Y" : ""),
                      (lassummary.has_extremely_serious_fluff(2) ? "Z" : ""));
                  json_sub_main["warnings"].push_back(buffer);
                } else {
                  fprintf(
                      file_out, "WARNING: there is extremely serious coordinate resolution fluff (x10000) in %s%s%s\012",
                      (lassummary.has_extremely_serious_fluff(0) ? "X" : ""), (lassummary.has_extremely_serious_fluff(1) ? "Y" : ""),
                      (lassummary.has_extremely_serious_fluff(2) ? "Z" : ""));
                }
              }
            }
          }
        }
        if (file_out && !no_returns) {
          if (json_out) {
            if (num_first_returns > 0) json_sub_main["number_of_first_returns"] = num_first_returns;
            if (num_intermediate_returns > 0) json_sub_main["number_of_intermediate_returns"] = num_intermediate_returns;
            if (num_last_returns > 0) json_sub_main["number_of_last_returns"] = num_last_returns;
            if (num_single_returns > 0) json_sub_main["number_of_single_returns"] = num_single_returns;
          } else {
            fprintf(file_out, "number of first returns:        %lld\012", num_first_returns);
            fprintf(file_out, "number of intermediate returns: %lld\012", num_intermediate_returns);
            fprintf(file_out, "number of last returns:         %lld\012", num_last_returns);
            fprintf(file_out, "number of single returns:       %lld\012", num_single_returns);
          }
        }
        if (file_out && lasoccupancygrid) {
          if (num_last_returns) {
            JsonObject json_lasoccupancygrid;

            if (horizontal_units == 9001) {
              if (json_out) {
                json_lasoccupancygrid["covered_area"]["description"] = "covered area in square meters/kilometers";
                json_lasoccupancygrid["covered_area"]["square_meters"] = 4 * lasoccupancygrid->get_num_occupied();
                json_lasoccupancygrid["covered_area"]["kilometers"] = round_to_decimals(0.000004 * lasoccupancygrid->get_num_occupied(), 2);
                json_lasoccupancygrid["point_density"]["description"] = "point density per square meter";
                json_lasoccupancygrid["point_density"]["all_returns"] =
                    round_to_decimals(((F64)num_all_returns / (4.0 * lasoccupancygrid->get_num_occupied())), 2);
                json_lasoccupancygrid["point_density"]["last_only"] =
                    round_to_decimals(((F64)num_last_returns / (4.0 * lasoccupancygrid->get_num_occupied())), 2);
                json_lasoccupancygrid["spacing"]["description"] = "spacing in meters";
                json_lasoccupancygrid["spacing"]["all_returns"] =
                    round_to_decimals(sqrt(4.0 * lasoccupancygrid->get_num_occupied() / (F64)num_all_returns), 2);
                json_lasoccupancygrid["spacing"]["last_only"] =
                    round_to_decimals(sqrt(4.0 * lasoccupancygrid->get_num_occupied() / (F64)num_last_returns), 2);
              } else {
                fprintf(
                    file_out, "covered area in square meters/kilometers: %d/%.2f\012", 4 * lasoccupancygrid->get_num_occupied(),
                    0.000004 * lasoccupancygrid->get_num_occupied());
                fprintf(
                    file_out, "point density: all returns %.2f last only %.2f (per square meter)\012",
                    ((F64)num_all_returns / (4.0 * lasoccupancygrid->get_num_occupied())),
                    ((F64)num_last_returns / (4.0 * lasoccupancygrid->get_num_occupied())));
                fprintf(
                    file_out, "      spacing: all returns %.2f last only %.2f (in meters)\012",
                    sqrt(4.0 * lasoccupancygrid->get_num_occupied() / (F64)num_all_returns),
                    sqrt(4.0 * lasoccupancygrid->get_num_occupied() / (F64)num_last_returns));
              }
            } else if (horizontal_units == 9002) {
              if (json_out) {
                json_lasoccupancygrid["covered_area"]["description"] = "covered area in square feet/miles";
                json_lasoccupancygrid["covered_area"]["square_feet"] = 36 * lasoccupancygrid->get_num_occupied();
                json_lasoccupancygrid["covered_area"]["miles"] = round_to_decimals(1.2913223e-6 * lasoccupancygrid->get_num_occupied(), 2);
                json_lasoccupancygrid["point_density"]["description"] = "point density per square foot";
                json_lasoccupancygrid["point_density"]["all_returns"] =
                    round_to_decimals(((F64)num_all_returns / (36.0 * lasoccupancygrid->get_num_occupied())), 2);
                json_lasoccupancygrid["point_density"]["last_only"] =
                    round_to_decimals(((F64)num_last_returns / (36.0 * lasoccupancygrid->get_num_occupied())), 2);
                json_lasoccupancygrid["spacing"]["description"] = "spacing in feet";
                json_lasoccupancygrid["spacing"]["all_returns"] =
                    round_to_decimals(sqrt(36.0 * lasoccupancygrid->get_num_occupied() / (F64)num_all_returns), 2);
                json_lasoccupancygrid["spacing"]["last_only"] =
                    round_to_decimals(sqrt(36.0 * lasoccupancygrid->get_num_occupied() / (F64)num_last_returns), 2);
              } else {
                fprintf(
                    file_out, "covered area in square feet/miles: %d/%.2f\012", 36 * lasoccupancygrid->get_num_occupied(),
                    1.2913223e-6 * lasoccupancygrid->get_num_occupied());
                fprintf(
                    file_out, "point density: all returns %.2f last only %.2f (per square foot)\012",
                    ((F64)num_all_returns / (36.0 * lasoccupancygrid->get_num_occupied())),
                    ((F64)num_last_returns / (36.0 * lasoccupancygrid->get_num_occupied())));
                fprintf(
                    file_out, "      spacing: all returns %.2f last only %.2f (in feet)\012",
                    sqrt(36.0 * lasoccupancygrid->get_num_occupied() / (F64)num_all_returns),
                    sqrt(36.0 * lasoccupancygrid->get_num_occupied() / (F64)num_last_returns));
              }
            } else if (horizontal_units == 9003) {
              if (json_out) {
                json_lasoccupancygrid["covered_area"]["description"] = "covered area in square survey feet";
                json_lasoccupancygrid["covered_area"]["square_survey_feet"] = 36 * lasoccupancygrid->get_num_occupied();
                json_lasoccupancygrid["point_density"]["description"] = "point density per square survey foot";
                json_lasoccupancygrid["point_density"]["all_returns"] =
                    round_to_decimals(((F64)num_all_returns / (36.0 * lasoccupancygrid->get_num_occupied())), 2);
                json_lasoccupancygrid["point_density"]["last_only"] =
                    round_to_decimals(((F64)num_last_returns / (36.0 * lasoccupancygrid->get_num_occupied())), 2);
                json_lasoccupancygrid["spacing"]["description"] = "spacing in survey feet";
                json_lasoccupancygrid["spacing"]["all_returns"] =
                    round_to_decimals(sqrt(36.0 * lasoccupancygrid->get_num_occupied() / (F64)num_all_returns), 2);
                json_lasoccupancygrid["spacing"]["last_only"] =
                    round_to_decimals(sqrt(36.0 * lasoccupancygrid->get_num_occupied() / (F64)num_last_returns), 2);
              } else {
                fprintf(file_out, "covered area in square survey feet: %d\012", 36 * lasoccupancygrid->get_num_occupied());
                fprintf(
                    file_out, "point density: all returns %.2f last only %.2f (per square survey foot)\012",
                    ((F64)num_all_returns / (36.0 * lasoccupancygrid->get_num_occupied())),
                    ((F64)num_last_returns / (36.0 * lasoccupancygrid->get_num_occupied())));
                fprintf(
                    file_out, "      spacing: all returns %.2f last only %.2f (in survey feet)\012",
                    sqrt(36.0 * lasoccupancygrid->get_num_occupied() / (F64)num_all_returns),
                    sqrt(36.0 * lasoccupancygrid->get_num_occupied() / (F64)num_last_returns));
              }
            } else {
              if (json_out) {
                json_lasoccupancygrid["covered_area"]["description"] = "covered area in square units/kilounits";
                json_lasoccupancygrid["covered_area"]["square_units"] = 4 * lasoccupancygrid->get_num_occupied();
                json_lasoccupancygrid["covered_area"]["kilounits"] = round_to_decimals(0.000004 * lasoccupancygrid->get_num_occupied(), 2);
                json_lasoccupancygrid["point_density"]["description"] = "point density per square units";
                json_lasoccupancygrid["point_density"]["all_returns"] =
                    round_to_decimals(((F64)num_all_returns / (4.0 * lasoccupancygrid->get_num_occupied())), 2);
                json_lasoccupancygrid["point_density"]["last_only"] =
                    round_to_decimals(((F64)num_last_returns / (4.0 * lasoccupancygrid->get_num_occupied())), 2);
                json_lasoccupancygrid["spacing"]["description"] = "spacing in units";
                json_lasoccupancygrid["spacing"]["all_returns"] =
                    round_to_decimals(sqrt(4.0 * lasoccupancygrid->get_num_occupied() / (F64)num_all_returns), 2);
                json_lasoccupancygrid["spacing"]["last_only"] =
                    round_to_decimals(sqrt(4.0 * lasoccupancygrid->get_num_occupied() / (F64)num_last_returns), 2);
              } else {
                fprintf(
                    file_out, "covered area in square units/kilounits: %d/%.2f\012", 4 * lasoccupancygrid->get_num_occupied(),
                    0.000004 * lasoccupancygrid->get_num_occupied());
                fprintf(
                    file_out, "point density: all returns %.2f last only %.2f (per square units)\012",
                    ((F64)num_all_returns / (4.0 * lasoccupancygrid->get_num_occupied())),
                    ((F64)num_last_returns / (4.0 * lasoccupancygrid->get_num_occupied())));
                fprintf(
                    file_out, "      spacing: all returns %.2f last only %.2f (in units)\012",
                    sqrt(4.0 * lasoccupancygrid->get_num_occupied() / (F64)num_all_returns),
                    sqrt(4.0 * lasoccupancygrid->get_num_occupied() / (F64)num_last_returns));
              }
            }
            if (json_out && !json_lasoccupancygrid.is_null()) json_sub_main["las_occupancy_grid"] = json_lasoccupancygrid;
          }
          delete lasoccupancygrid;
        }
      }

      // PROJ CRS Representations and information query
      if (file_out && geoprojectionconverter.is_proj_request) {
        JsonObject json_proj_info;
        // Try to generate the CRS PROJ object from the input file header information
        if (lasreader->header.vlr_geo_ogc_wkt) {  // try to get it from the OGC WKT string
          geoprojectionconverter.set_proj_crs_with_file_header_wkt(lasreader->header.vlr_geo_ogc_wkt, true);
        } else if (lasreader->header.vlr_geo_keys) {  // if no WKT exist in file header try keo_keys
          geoprojectionconverter.set_projection_from_geo_keys(
              lasreader->header.vlr_geo_keys[0].number_of_keys, (GeoProjectionGeoKeys*)lasreader->header.vlr_geo_key_entries,
              lasreader->header.vlr_geo_ascii_params, lasreader->header.vlr_geo_double_params);
          geoprojectionconverter.reset_projection();

          if (geoprojectionconverter.source_header_epsg > 0) {
            // create the PROJ object for the proj info query here
            geoprojectionconverter.set_proj_crs_with_epsg(geoprojectionconverter.source_header_epsg, true);
          } else {
            laserror("No valid CRS could be extracted from the header information of the source file.");
          }
        } else {
          laserror("No file header information could be found to identify the CRS.");
        }
        const char* info_content = nullptr;
        const char* proj_crs_infos = nullptr;

        if (json_out) {
          json_proj_info["description"] = "PROJ Coordinate Reference System (CRS) Representation and Information";
        } else {
          fprintf(file_out, "PROJ Coordinate Reference System (CRS) Representation and Information \n");
        }
        // Query the WKT representation of the CRS
        if (geoprojectionconverter.projParameters.proj_info_arg_contains("wkt")) {
          proj_crs_infos = geoprojectionconverter.projParameters.get_wkt_representation(true);
          info_content = indent_text(proj_crs_infos, "  ");

          if (info_content == nullptr || *info_content == '\0') {
            LASMessage(LAS_WARNING, "the content of the wkt representation of the CRS could not be generated");
          } else {
            if (json_out) {
              json_proj_info["wkt"] = info_content;
            } else {
              fprintf(file_out, "WKT representation of the CRS: \n");
              fprintf(file_out, "%s \n", info_content);
            }
          }
          delete[] info_content;
          info_content = nullptr;
        }
        // Query the PROJJSON representation of the CRS
        if (geoprojectionconverter.projParameters.proj_info_arg_contains("js")) {
          proj_crs_infos = geoprojectionconverter.projParameters.get_json_representation(true);
          info_content = indent_text(proj_crs_infos, "  ");

          if (info_content == nullptr || *info_content == '\0') {
            LASMessage(LAS_WARNING, "the content of the json representation of the CRS could not be generated");
          } else {
            if (json_out) {
              json_proj_info["proj_json"] = info_content;
            } else {
              fprintf(file_out, "Json representation of the CRS: \n");
              fprintf(file_out, "%s \n", info_content);
            }
          }
          delete[] info_content;
          info_content = nullptr;
        }
        // Query the PROJ string representation of the CRS
        if (geoprojectionconverter.projParameters.proj_info_arg_contains("str")) {
          proj_crs_infos = geoprojectionconverter.projParameters.get_projString_representation(true);
          info_content = indent_text(proj_crs_infos, "  ");

          if (info_content == nullptr || *info_content == '\0') {
            LASMessage(LAS_WARNING, "the content of the PROJ string representation of the CRS could not be generated");
          } else {
            if (json_out) {
              json_proj_info["proj_string"] = info_content;
            } else {
              fprintf(file_out, "PROJ string representation of the CRS: \n");
              fprintf(file_out, "%s \n", info_content);
            }
          }
          delete[] info_content;
          info_content = nullptr;
        }
        // Query the EPSG code of the CRS
        if (geoprojectionconverter.projParameters.proj_info_arg_contains("epsg")) {
          proj_crs_infos = geoprojectionconverter.projParameters.get_epsg_representation(true);
          info_content = indent_text(proj_crs_infos, "  ");

          if (info_content == nullptr || *info_content == '\0') {
            LASMessage(LAS_WARNING, "the content of the epsg representation of the CRS could not be generated");
          } else {
            if (json_out) {
              json_proj_info["epsg"] = info_content;
            } else {
              fprintf(file_out, "Epsg-Code representation of the CRS: \n");
              fprintf(file_out, "%s \n", info_content);
            }
          }
          delete[] info_content;
          info_content = nullptr;
        }
        // Query the ellipsoidal informations of the CRS
        if (geoprojectionconverter.projParameters.proj_info_arg_contains("el")) {
          proj_crs_infos = geoprojectionconverter.projParameters.get_ellipsoid_info(true);
          info_content = indent_text(proj_crs_infos, "  ");

          if (info_content == nullptr || *info_content == '\0') {
            LASMessage(LAS_WARNING, "the content of the ellipsoid information could not be generated");
          } else {
            if (json_out) {
              json_proj_info["ellipsoid"] = info_content;
            } else {
              fprintf(file_out, "Ellipsoid of the CRS: \n");
              fprintf(file_out, "%s \n", info_content);
            }
          }
          delete[] info_content;
          info_content = nullptr;
        }
        // Query the datum informations of the CRS
        if (geoprojectionconverter.projParameters.proj_info_arg_contains("datum")) {
          proj_crs_infos = geoprojectionconverter.projParameters.get_datum_info(true);
          info_content = indent_text(proj_crs_infos, "  ");

          if (info_content == nullptr || *info_content == '\0') {
            LASMessage(LAS_WARNING, "the content of the datum information could not be generated");
          } else {
            if (json_out) {
              json_proj_info["datum"] = info_content;
            } else {
              fprintf(file_out, "Datum of the CRS: \n");
              fprintf(file_out, "%s \n", info_content);
            }
          }
          delete[] info_content;
          info_content = nullptr;
        }
        // Query the coordinate system informations of the CRS
        if (geoprojectionconverter.projParameters.proj_info_arg_contains("cs")) {
          proj_crs_infos = geoprojectionconverter.projParameters.get_coord_system_info(true);
          info_content = indent_text(proj_crs_infos, "  ");

          if (info_content == nullptr || *info_content == '\0') {
            LASMessage(LAS_WARNING, "the content of the CRS information could not be generated");
          } else {
            if (json_out) {
              json_proj_info["coordinate_system"] = info_content;
            } else {
              fprintf(file_out, "Coordinate system of the CRS: \n");
              fprintf(file_out, "%s \n", info_content);
            }
          }
          delete[] info_content;
          info_content = nullptr;
        }
        if (json_out && !json_proj_info.is_null()) json_sub_main["crs_infos"] = json_proj_info;
      }

      lasreader->close();

      FILE* file = 0;

      if (repair_bb || repair_counters) {
        if (lasreadopener.is_piped()) {
          laserror("cannot repair header of piped input");
          repair_bb = repair_counters = false;
        } else if (lasreadopener.is_merged()) {
          laserror("cannot repair header of merged input");
          repair_bb = repair_counters = false;
        } else if (lasreadopener.is_buffered()) {
          laserror("cannot repair header of buffered input");
          repair_bb = repair_counters = false;
        } else if (lasreader->get_format() > LAS_TOOLS_FORMAT_LAZ) {
          laserror("can only repair header for LAS or LAZ files, not for '%s'", lasreadopener.get_file_name());
          repair_bb = repair_counters = false;
        }
        file = LASfopen(lasreadopener.get_file_name(), "rb+");
        if (file == 0) {
          laserror("could not reopen file '%s' for repair of header", lasreadopener.get_file_name());
          repair_bb = repair_counters = false;
        }
      }

      if (check_points) {
        JsonObject json_point_number;
        // check number_of_point_records
        if ((lasheader->point_data_format < 6) && (lassummary.number_of_point_records != lasheader->number_of_point_records)) {
          if (repair_counters) {
            if (lassummary.number_of_point_records <= U32_MAX) {
              U32 number_of_point_records = (U32)lassummary.number_of_point_records;
              fseek(file, 107, SEEK_SET);
              fwrite(&number_of_point_records, sizeof(U32), 1, file);
              if (file_out) {
                if (json_out) {
                  char buffer[256];
                  snprintf(
                      buffer, sizeof(buffer), "WARNING: real number of point records (%u) is different from header entry (%u). it was repaired. \n",
                      number_of_point_records, lasheader->number_of_point_records);
                  json_point_number["warnings"].push_back(buffer);
                } else {
                  fprintf(
                      file_out, "WARNING: real number of point records (%u) is different from header entry (%u). it was repaired. \n",
                      number_of_point_records, lasheader->number_of_point_records);
                }
              }
            } else if (lasheader->version_minor < 4) {
              if (file_out) {
                if (json_out) {
                  char buffer[256];
                  snprintf(
                      buffer, sizeof(buffer), "WARNING: real number of point records (%lld) exceeds 4,294,967,295. cannot repair. too big.\n",
                      lassummary.number_of_point_records);
                  json_point_number["warnings"].push_back(buffer);
                } else {
                  fprintf(
                      file_out, "WARNING: real number of point records (%lld) exceeds 4,294,967,295. cannot repair. too big.\n",
                      lassummary.number_of_point_records);
                }
              }
            } else if (lasheader->number_of_point_records != 0) {
              U32 number_of_point_records = 0;
              fseek(file, 107, SEEK_SET);
              fwrite(&number_of_point_records, sizeof(U32), 1, file);
              if (file_out) {
                if (json_out) {
                  char buffer[256];
                  snprintf(
                      buffer, sizeof(buffer),
                      "WARNING: real number of point records (%lld) exceeds 4,294,967,295. but header entry is %u instead zero. it was repaired.\n",
                      lassummary.number_of_point_records, lasheader->number_of_point_records);
                  json_point_number["warnings"].push_back(buffer);
                } else {
                  fprintf(
                      file_out,
                      "WARNING: real number of point records (%lld) exceeds 4,294,967,295. but header entry is %u instead zero. it was repaired.\n",
                      lassummary.number_of_point_records, lasheader->number_of_point_records);
                }
              }
            } else {
              if (file_out) {
                if (json_out) {
                  json_point_number["info"] = "number of point records in header is correct";
                } else {
                  fprintf(file_out, "number of point records in header is correct.\n");
                }
              }
            }
          } else {
            if (!no_warnings && file_out) {
              if (lassummary.number_of_point_records <= U32_MAX) {
                if (json_out) {
                  char buffer[256];
                  snprintf(
                      buffer, sizeof(buffer), "WARNING: real number of point records (%lld) is different from header entry (%u).\n",
                      lassummary.number_of_point_records, lasheader->number_of_point_records);
                  json_point_number["warnings"].push_back(buffer);
                } else {
                  fprintf(
                      file_out, "WARNING: real number of point records (%lld) is different from header entry (%u).\n",
                      lassummary.number_of_point_records, lasheader->number_of_point_records);
                }
              } else if (lasheader->version_minor < 4) {
                if (json_out) {
                  char buffer[256];
                  snprintf(
                      buffer, sizeof(buffer), "WARNING: real number of point records (%lld) exceeds 4,294,967,295.\n",
                      lassummary.number_of_point_records);
                  json_point_number["warnings"].push_back(buffer);
                } else {
                  fprintf(file_out, "WARNING: real number of point records (%lld) exceeds 4,294,967,295.\n", lassummary.number_of_point_records);
                }
              } else if (lasheader->number_of_point_records != 0) {
                if (json_out) {
                  char buffer[256];
                  snprintf(
                      buffer, sizeof(buffer),
                      "WARNING: real number of point records (%lld) exceeds 4,294,967,295. but header entry is %u instead of zero.\n",
                      lassummary.number_of_point_records, lasheader->number_of_point_records);
                  json_point_number["warnings"].push_back(buffer);
                } else {
                  fprintf(
                      file_out, "WARNING: real number of point records (%lld) exceeds 4,294,967,295. but header entry is %u instead of zero.\n",
                      lassummary.number_of_point_records, lasheader->number_of_point_records);
                }
              }
            }
          }
        } else if ((lasheader->point_data_format >= 6) && (lasheader->number_of_point_records != 0)) {
          if (repair_counters) {
            U32 number_of_point_records = 0;
            fseek(file, 107, SEEK_SET);
            fwrite(&number_of_point_records, sizeof(U32), 1, file);
          }
          if (!no_warnings && file_out) {
            if (json_out) {
              char buffer[256];
              snprintf(
                  buffer, sizeof(buffer), "WARNING: point type is %d but (legacy) number of point records in header is %u instead zero.%s\n",
                  lasheader->point_data_format, lasheader->number_of_point_records, (repair_counters ? "it was repaired." : ""));
              json_point_number["warnings"].push_back(buffer);
            } else {
              fprintf(
                  file_out, "WARNING: point type is %d but (legacy) number of point records in header is %u instead zero.%s\n",
                  lasheader->point_data_format, lasheader->number_of_point_records, (repair_counters ? "it was repaired." : ""));
            }
          }
        } else {
          if (repair_counters) {
            if (file_out) {
              if (json_out) {
                json_point_number["info"] = "number of point records in header is correct";
              } else {
                fprintf(file_out, "number of point records in header is correct.\n");
              }
            }
          }
        }
        if (json_out && !json_point_number.is_null()) json_sub_main["number_of_point_records"] = json_point_number;

        // check extended_number_of_point_records

        if (lasheader->version_minor > 3) {
          JsonObject json_point_extended_number;

          if (lassummary.number_of_point_records != (I64)lasheader->extended_number_of_point_records) {
            if (repair_counters) {
              I64 extended_number_of_point_records = lassummary.number_of_point_records;
              fseek(file, 235 + 12, SEEK_SET);
              fwrite(&extended_number_of_point_records, sizeof(I64), 1, file);
            }
            if (!no_warnings && file_out) {
              if (json_out) {
                char buffer[256];
                snprintf(
                    buffer, sizeof(buffer), "WARNING: real number of point records (%lld) is different from extended header entry (%lld).%s\n",
                    lassummary.number_of_point_records, lasheader->extended_number_of_point_records, (repair_counters ? " it was repaired." : ""));
                json_point_extended_number["warnings"].push_back(buffer);
              } else {
                fprintf(
                    file_out, "WARNING: real number of point records (%lld) is different from extended header entry (%lld).%s\n",
                    lassummary.number_of_point_records, lasheader->extended_number_of_point_records, (repair_counters ? " it was repaired." : ""));
              }
            }
          } else {
            if (repair_counters) {
              if (file_out) {
                if (json_out) {
                  json_point_extended_number["info"] = "extended number of point records in header is correct";
                } else {
                  fprintf(file_out, "extended number of point records in header is correct.\n");
                }
              }
            }
          }
          if (json_out && !json_point_extended_number.is_null()) json_sub_main["extended_number_of_point_records"] = json_point_extended_number;
        }

        // check number_of_points_by_return[5]
        bool was_set = false;
        for (i = 1; i < 6; i++)
          if (lasheader->number_of_points_by_return[i - 1]) was_set = true;

        bool wrong_entry = false;
        JsonObject json_point_by_return;
        U32 number_of_points_by_return[5];

        for (i = 1; i < 6; i++) {
          if ((lasheader->point_data_format < 6) &&
              ((I64)(lasheader->number_of_points_by_return[i - 1]) != lassummary.number_of_points_by_return[i])) {
            if (lassummary.number_of_points_by_return[i] <= U32_MAX) {
              number_of_points_by_return[i - 1] = (U32)lassummary.number_of_points_by_return[i];
              wrong_entry = true;
              if (!no_warnings && file_out) {
                if (was_set) {
                  if (json_out) {
                    char buffer[256];
                    snprintf(
                        buffer, sizeof(buffer),
                        "WARNING: for return %d real number of points by return (%u) is different from header entry (%u).%s\n", i,
                        number_of_points_by_return[i - 1], lasheader->number_of_points_by_return[i - 1],
                        (repair_counters ? " it was repaired." : ""));
                    json_point_by_return["warnings"].push_back(buffer);
                  } else {
                    fprintf(
                        file_out, "WARNING: for return %d real number of points by return (%u) is different from header entry (%u).%s\n", i,
                        number_of_points_by_return[i - 1], lasheader->number_of_points_by_return[i - 1],
                        (repair_counters ? " it was repaired." : ""));
                  }
                } else {
                  if (json_out) {
                    char buffer[256];
                    snprintf(
                        buffer, sizeof(buffer), "WARNING: for return %d real number of points by return is %u but header entry was not set.%s\n", i,
                        number_of_points_by_return[i - 1], (repair_counters ? " it was repaired." : ""));
                    json_point_by_return["warnings"].push_back(buffer);
                  } else {
                    fprintf(
                        file_out, "WARNING: for return %d real number of points by return is %u but header entry was not set.%s\n", i,
                        number_of_points_by_return[i - 1], (repair_counters ? " it was repaired." : ""));
                  }
                }
              }
            } else if (lasheader->version_minor < 4) {
              if (!no_warnings && file_out) {
                if (json_out) {
                  char buffer[256];
                  snprintf(
                      buffer, sizeof(buffer), "WARNING: for return %d real number of points by return (%lld) exceeds 4,294,967,295.%s\n", i,
                      lassummary.number_of_points_by_return[i], (repair_counters ? " cannot repair. too big." : ""));
                  json_point_by_return["warnings"].push_back(buffer);
                } else {
                  fprintf(
                      file_out, "WARNING: for return %d real number of points by return (%lld) exceeds 4,294,967,295.%s\n", i,
                      lassummary.number_of_points_by_return[i], (repair_counters ? " cannot repair. too big." : ""));
                }
              }
            } else if (lasheader->number_of_points_by_return[i - 1] != 0) {
              number_of_points_by_return[i - 1] = 0;
              wrong_entry = true;
              if (!no_warnings && file_out) {
                if (json_out) {
                  char buffer[256];
                  snprintf(
                      buffer, sizeof(buffer),
                      "WARNING: for return %d real number of points by return (%lld) exceeds 4,294,967,295. but header entry is %u instead zero.%s\n",
                      i, lassummary.number_of_points_by_return[i], lasheader->number_of_points_by_return[i - 1],
                      (repair_counters ? " it was repaired." : ""));
                  json_point_by_return["warnings"].push_back(buffer);
                } else {
                  fprintf(
                      file_out,
                      "WARNING: for return %d real number of points by return (%lld) exceeds 4,294,967,295. but header entry is %u instead zero.%s\n",
                      i, lassummary.number_of_points_by_return[i], lasheader->number_of_points_by_return[i - 1],
                      (repair_counters ? " it was repaired." : ""));
                }
              }
            } else {
              number_of_points_by_return[i - 1] = 0;
            }
          } else if ((lasheader->point_data_format >= 6) && (lasheader->number_of_points_by_return[i - 1] != 0)) {
            number_of_points_by_return[i - 1] = 0;
            wrong_entry = true;
            if (!no_warnings && file_out) {
              if (json_out) {
                char buffer[256];
                snprintf(
                    buffer, sizeof(buffer),
                    "WARNING: point type is %d but (legacy) number of points by return [%d] in header is %u instead zero.%s\n",
                    lasheader->point_data_format, i, lasheader->number_of_points_by_return[i - 1], (repair_counters ? "it was repaired." : ""));
                json_point_by_return["warnings"].push_back(buffer);
              } else {
                fprintf(
                    file_out, "WARNING: point type is %d but (legacy) number of points by return [%d] in header is %u instead zero.%s\n",
                    lasheader->point_data_format, i, lasheader->number_of_points_by_return[i - 1], (repair_counters ? "it was repaired." : ""));
              }
            }
          } else {
            number_of_points_by_return[i - 1] = (U32)lassummary.number_of_points_by_return[i];
          }
        }

        if (repair_counters) {
          if (wrong_entry) {
            fseek(file, 111, SEEK_SET);
            fwrite(&(number_of_points_by_return[0]), sizeof(U32), 5, file);
          } else if (file_out) {
            if (json_out) {
              json_point_by_return["info"] = "number of points by return in header is correct";
            } else {
              fprintf(file_out, "number of points by return in header is correct.\n");
            }
          }
        }
        // check extended_number_of_points_by_return[15]
        JsonObject json_point_extended_by_return;

        if (lasheader->version_minor > 3) {
          bool was_set = false;
          for (i = 1; i < 15; i++)
            if (lasheader->extended_number_of_points_by_return[i - 1]) was_set = true;

          bool wrong_entry = false;

          I64 extended_number_of_points_by_return[15];

          for (i = 1; i < 16; i++) {
            extended_number_of_points_by_return[i - 1] = lassummary.number_of_points_by_return[i];
            if ((I64)lasheader->extended_number_of_points_by_return[i - 1] != lassummary.number_of_points_by_return[i]) {
              wrong_entry = true;
              if (!no_warnings && file_out) {
                if (was_set) {
                  if (json_out) {
                    char buffer[256];
                    snprintf(
                        buffer, sizeof(buffer),
                        "WARNING: real extended number of points by return [%d] is %lld - different from header entry %lld.%s\n", i,
                        lassummary.number_of_points_by_return[i], lasheader->extended_number_of_points_by_return[i - 1],
                        (repair_counters ? " it was repaired." : ""));
                    json_point_extended_by_return["warnings"].push_back(buffer);
                  } else {
                    fprintf(
                        file_out, "WARNING: real extended number of points by return [%d] is %lld - different from header entry %lld.%s\n", i,
                        lassummary.number_of_points_by_return[i], lasheader->extended_number_of_points_by_return[i - 1],
                        (repair_counters ? " it was repaired." : ""));
                  }
                } else {
                  if (json_out) {
                    char buffer[256];
                    snprintf(
                        buffer, sizeof(buffer), "WARNING: real extended number of points by return [%d] is %lld but header entry was not set.%s\n", i,
                        lassummary.number_of_points_by_return[i], (repair_counters ? " it was repaired." : ""));
                    json_point_extended_by_return["warnings"].push_back(buffer);
                  } else {
                    fprintf(
                        file_out, "WARNING: real extended number of points by return [%d] is %lld but header entry was not set.%s\n", i,
                        lassummary.number_of_points_by_return[i], (repair_counters ? " it was repaired." : ""));
                  }
                }
              }
            }
          }

          if (repair_counters) {
            if (wrong_entry) {
              fseek(file, 235 + 20, SEEK_SET);
              fwrite(&(extended_number_of_points_by_return[0]), sizeof(I64), 15, file);
            } else if (file_out) {
              if (json_out) {
                json_point_extended_by_return["info"] = "number of points by return in header is correct";
              } else {
                fprintf(file_out, "extended number of points by return in header is correct.\n");
              }
            }
          }
          if (json_out && !json_point_extended_by_return.is_null())
            json_sub_main["extended_number_of_points_by_return"] = json_point_extended_by_return;
        }

        if (!no_warnings && file_out && !no_returns) {
          if (lassummary.number_of_points_by_return[0]) {
            if (json_out) {
              char buffer[256];
              snprintf(
                  buffer, sizeof(buffer), "WARNING: there %s %lld point%s with return number 0\n",
                  (lassummary.number_of_points_by_return[0] > 1 ? "are" : "is"), lassummary.number_of_points_by_return[0],
                  (lassummary.number_of_points_by_return[0] > 1 ? "s" : ""));
              json_point_by_return["warnings"].push_back(buffer);
            } else {
              fprintf(
                  file_out, "WARNING: there %s %lld point%s with return number 0\n", (lassummary.number_of_points_by_return[0] > 1 ? "are" : "is"),
                  lassummary.number_of_points_by_return[0], (lassummary.number_of_points_by_return[0] > 1 ? "s" : ""));
            }
          }
          if (lasheader->version_minor < 4) {
            if (lassummary.number_of_points_by_return[6]) {
              if (json_out) {
                char buffer[256];
                snprintf(
                    buffer, sizeof(buffer), "WARNING: there %s %lld point%s with return number 6\n",
                    (lassummary.number_of_points_by_return[6] > 1 ? "are" : "is"), lassummary.number_of_points_by_return[6],
                    (lassummary.number_of_points_by_return[6] > 1 ? "s" : ""));
                json_point_by_return["warnings"].push_back(buffer);
              } else {
                fprintf(
                    file_out, "WARNING: there %s %lld point%s with return number 6\n", (lassummary.number_of_points_by_return[6] > 1 ? "are" : "is"),
                    lassummary.number_of_points_by_return[6], (lassummary.number_of_points_by_return[6] > 1 ? "s" : ""));
              }
            }
            if (lassummary.number_of_points_by_return[7]) {
              if (json_out) {
                char buffer[256];
                snprintf(
                    buffer, sizeof(buffer), "WARNING: there %s %lld point%s with return number 7\n",
                    (lassummary.number_of_points_by_return[7] > 1 ? "are" : "is"), lassummary.number_of_points_by_return[7],
                    (lassummary.number_of_points_by_return[7] > 1 ? "s" : ""));
                json_point_by_return["warnings"].push_back(buffer);
              } else {
                fprintf(
                    file_out, "WARNING: there %s %lld point%s with return number 7\n", (lassummary.number_of_points_by_return[7] > 1 ? "are" : "is"),
                    lassummary.number_of_points_by_return[7], (lassummary.number_of_points_by_return[7] > 1 ? "s" : ""));
              }
            }
          }

          wrong_entry = false;
          if (lasheader->version_minor > 3) {
            for (i = 1; i < 16; i++)
              if (lassummary.number_of_returns[i]) wrong_entry = true;
            if (wrong_entry) {
              if (json_out) {
                for (i = 1; i < 16; i++) json_point_by_return["extended_number_of_returns_of_given_pulse"].push_back(lassummary.number_of_returns[i]);
              } else {
                fprintf(file_out, "overview over extended number of returns of given pulse:");
                for (i = 1; i < 16; i++) fprintf(file_out, " %lld", lassummary.number_of_returns[i]);
                fprintf(file_out, "\n");
              }
            }
          } else {
            for (i = 1; i < 8; i++)
              if (lassummary.number_of_returns[i]) wrong_entry = true;
            if (wrong_entry) {
              if (json_out) {
                for (i = 1; i < 8; i++) json_point_by_return["number_of_returns_of_given_pulse"].push_back(lassummary.number_of_returns[i]);
              } else {
                fprintf(file_out, "overview over number of returns of given pulse:");
                for (i = 1; i < 8; i++) fprintf(file_out, " %lld", lassummary.number_of_returns[i]);
                fprintf(file_out, "\n");
              }
            }
          }

          if (lassummary.number_of_returns[0]) {
            if (json_out) {
              char buffer[256];
              snprintf(
                  buffer, sizeof(buffer), "WARNING: there are %lld points with a number of returns of given pulse of 0\n",
                  lassummary.number_of_returns[0]);
              json_point_by_return["warnings"].push_back(buffer);
            } else {
              fprintf(file_out, "WARNING: there are %lld points with a number of returns of given pulse of 0\n", lassummary.number_of_returns[0]);
            }
          }
        }
        if (json_out && !json_point_by_return.is_null()) json_sub_main["points_by_return"] = json_point_by_return;

        if (file_out && !no_min_max) {
          JsonObject json_histogram_classification;
          wrong_entry = false;
          for (i = 0; i < 32; i++)
            if (lassummary.classification[i]) wrong_entry = true;
          if (lassummary.flagged_synthetic || lassummary.flagged_keypoint || lassummary.flagged_withheld) wrong_entry = true;

          if (wrong_entry) {
            if (!json_out) fprintf(file_out, "histogram of classification of points:\n");
            for (i = 0; i < 32; i++) {
              if (lassummary.classification[i]) {
                if (json_out) {
                  JsonObject json_classification;

                  json_classification["id"] = lassummary.classification[i];
                  json_classification["type"] = LASpointClassification[i];
                  json_classification["index"] = i;
                  json_histogram_classification["classification"].push_back(json_classification);
                } else {
                  fprintf(file_out, " %15lld  %s (%u)\n", lassummary.classification[i], LASpointClassification[i], i);
                }
              }
            }
            if (lassummary.flagged_synthetic) {
              if (json_out) {
                json_histogram_classification["flagged_as_synthetic"]["count"] = lassummary.flagged_synthetic;
              } else {
                fprintf(file_out, " +-> flagged as synthetic: %lld\n", lassummary.flagged_synthetic);
              }
              for (i = 0; i < 32; i++) {
                if (lassummary.flagged_synthetic_classification[i]) {
                  if (json_out) {
                    JsonObject json_synthetic_classification;

                    json_synthetic_classification["id"] = lassummary.flagged_synthetic_classification[i];
                    json_synthetic_classification["type"] = LASpointClassification[i];
                    json_synthetic_classification["index"] = i;
                    json_histogram_classification["flagged_as_synthetic"]["classification"].push_back(json_synthetic_classification);
                  } else {
                    fprintf(
                        file_out, "  +---> %15lld of those are %s (%u)\n", lassummary.flagged_synthetic_classification[i], LASpointClassification[i],
                        i);
                  }
                }
              }
              for (i = 32; i < 256; i++) {
                if (lassummary.flagged_synthetic_classification[i]) {
                  if (json_out) {
                    JsonObject json_synthetic_classification;

                    json_synthetic_classification["id"] = lassummary.flagged_synthetic_classification[i];
                    json_synthetic_classification["type"] = "classified";
                    json_synthetic_classification["index"] = i;
                    json_histogram_classification["flagged_as_synthetic"]["classification"].push_back(json_synthetic_classification);
                  } else {
                    fprintf(file_out, "  +---> %15lld  of those are classification (%u)\n", lassummary.flagged_synthetic_classification[i], i);
                  }
                }
              }
            }
            if (lassummary.flagged_keypoint) {
              if (json_out) {
                json_histogram_classification["flagged_as_keypoints"]["count"] = lassummary.flagged_keypoint;
              } else {
                fprintf(file_out, " +-> flagged as keypoints: %lld\n", lassummary.flagged_keypoint);
              }
              for (i = 0; i < 32; i++) {
                if (lassummary.flagged_keypoint_classification[i]) {
                  if (json_out) {
                    JsonObject json_keypoint_classification;

                    json_keypoint_classification["id"] = lassummary.flagged_keypoint_classification[i];
                    json_keypoint_classification["type"] = LASpointClassification[i];
                    json_keypoint_classification["index"] = i;
                    json_histogram_classification["flagged_as_keypoints"]["classification"].push_back(json_keypoint_classification);
                  } else {
                    fprintf(
                        file_out, "  +---> %15lld of those are %s (%u)\n", lassummary.flagged_keypoint_classification[i], LASpointClassification[i],
                        i);
                  }
                }
              }
              for (i = 32; i < 256; i++) {
                if (lassummary.flagged_keypoint_classification[i]) {
                  if (json_out) {
                    JsonObject json_keypoint_classification;

                    json_keypoint_classification["id"] = lassummary.flagged_keypoint_classification[i];
                    json_keypoint_classification["type"] = "classified";
                    json_keypoint_classification["index"] = i;
                    json_histogram_classification["flagged_as_keypoints"]["classification"].push_back(json_keypoint_classification);
                  } else {
                    fprintf(file_out, "  +---> %15lld  of those are classification (%u)\n", lassummary.flagged_keypoint_classification[i], i);
                  }
                }
              }
            }
            if (lassummary.flagged_withheld) {
              if (json_out) {
                json_histogram_classification["flagged_as_withheld"]["count"] = lassummary.flagged_withheld;
              } else {
                fprintf(file_out, " +-> flagged as withheld:  %lld\n", lassummary.flagged_withheld);
              }
              for (i = 0; i < 32; i++) {
                if (lassummary.flagged_withheld_classification[i]) {
                  if (json_out) {
                    JsonObject json_withheld_classification;

                    json_withheld_classification["id"] = lassummary.flagged_withheld_classification[i];
                    json_withheld_classification["type"] = LASpointClassification[i];
                    json_withheld_classification["index"] = i;
                    json_histogram_classification["flagged_as_withheld"]["classification"].push_back(json_withheld_classification);
                  } else {
                    fprintf(
                        file_out, "  +---> %15lld of those are %s (%u)\n", lassummary.flagged_withheld_classification[i], LASpointClassification[i],
                        i);
                  }
                }
              }
              for (i = 32; i < 256; i++) {
                if (lassummary.flagged_withheld_classification[i]) {
                  if (json_out) {
                    JsonObject json_withheld_classification;

                    json_withheld_classification["id"] = lassummary.flagged_withheld_classification[i];
                    json_withheld_classification["type"] = "classified";
                    json_withheld_classification["index"] = i;
                    json_histogram_classification["flagged_as_withheld"]["classification"].push_back(json_withheld_classification);
                  } else {
                    fprintf(file_out, "  +---> %15lld  of those are classification (%u)\n", lassummary.flagged_withheld_classification[i], i);
                  }
                }
              }
            }
          }
          JsonObject json_histogram_extended_classification;

          if (lasreader->point.extended_point_type) {
            if (lassummary.flagged_extended_overlap) {
              if (json_out) {
                json_histogram_classification["flagged_as_extended_overlap"]["count"] = lassummary.flagged_extended_overlap;
              } else {
                fprintf(file_out, " +-> flagged as extended overlap: %lld\n", lassummary.flagged_extended_overlap);
              }
              for (i = 0; i < 32; i++) {
                if (lassummary.flagged_extended_overlap_classification[i]) {
                  if (json_out) {
                    JsonObject json_extended_overlap_classification;

                    json_extended_overlap_classification["id"] = lassummary.flagged_extended_overlap_classification[i];
                    json_extended_overlap_classification["type"] = LASpointClassification[i];
                    json_extended_overlap_classification["index"] = i;
                    json_histogram_classification["flagged_as_extended_overlap"]["classification"].push_back(json_extended_overlap_classification);
                  } else {
                    fprintf(
                        file_out, "  +---> %15lld of those are %s (%u)\n", lassummary.flagged_extended_overlap_classification[i],
                        LASpointClassification[i], i);
                  }
                }
              }
              for (i = 32; i < 256; i++) {
                if (lassummary.flagged_extended_overlap_classification[i]) {
                  if (json_out) {
                    JsonObject json_extended_overlap_classification;

                    json_extended_overlap_classification["id"] = lassummary.flagged_extended_overlap_classification[i];
                    json_extended_overlap_classification["type"] = "classified";
                    json_extended_overlap_classification["index"] = i;
                    json_histogram_classification["flagged_as_extended_overlap"]["classification"].push_back(json_extended_overlap_classification);
                  } else {
                    fprintf(file_out, "  +---> %15lld  of those are classification (%u)\n", lassummary.flagged_extended_overlap_classification[i], i);
                  }
                }
              }
            }
            wrong_entry = false;
            for (i = 32; i < 256; i++) {
              if (lassummary.extended_classification[i]) wrong_entry = true;
            }

            if (wrong_entry) {
              if (!json_out) fprintf(file_out, "histogram of extended classification of points:\n");

              for (i = 32; i < 256; i++) {
                if (lassummary.extended_classification[i]) {
                  if (json_out) {
                    JsonObject json_extended_classification;

                    json_extended_classification["id"] = lassummary.extended_classification[i];
                    json_extended_classification["type"] = "extended classification";
                    json_extended_classification["index"] = i;
                    json_histogram_extended_classification["extended_classification"].push_back(json_extended_classification);
                  } else {
                    fprintf(file_out, " %15lld  extended classification (%u)\n", lassummary.extended_classification[i], i);
                  }
                }
              }
            }
          }
          if (json_out && !json_histogram_classification.is_null())
            json_sub_main["histogram_classification_of_points"] = json_histogram_classification;
          if (json_out && !json_histogram_extended_classification.is_null())
            json_sub_main["histogram_extended_classification_of_points"] = json_histogram_extended_classification;
        }

        if (lashistogram.active()) {
          lashistogram.report(file_out);
          lashistogram.reset();
        }
        double value;
        JsonObject json_bounding_box;

        if (repair_bb) {
          wrong_entry = false;
          if (lasheader->get_x(lassummary.max.get_X()) != lasheader->max_x) wrong_entry = true;
          if (lasheader->get_x(lassummary.min.get_X()) != lasheader->min_x) wrong_entry = true;
          if (lasheader->get_y(lassummary.max.get_Y()) != lasheader->max_y) wrong_entry = true;
          if (lasheader->get_y(lassummary.min.get_Y()) != lasheader->min_y) wrong_entry = true;
          if (lasheader->get_z(lassummary.max.get_Z()) != lasheader->max_z) wrong_entry = true;
          if (lasheader->get_z(lassummary.min.get_Z()) != lasheader->min_z) wrong_entry = true;
          if (wrong_entry) {
            fseek(file, 179, SEEK_SET);
            value = lasheader->get_x(lassummary.max.get_X());
            fwrite(&value, sizeof(double), 1, file);
            value = lasheader->get_x(lassummary.min.get_X());
            fwrite(&value, sizeof(double), 1, file);
            value = lasheader->get_y(lassummary.max.get_Y());
            fwrite(&value, sizeof(double), 1, file);
            value = lasheader->get_y(lassummary.min.get_Y());
            fwrite(&value, sizeof(double), 1, file);
            value = lasheader->get_z(lassummary.max.get_Z());
            fwrite(&value, sizeof(double), 1, file);
            value = lasheader->get_z(lassummary.min.get_Z());
            fwrite(&value, sizeof(double), 1, file);
            if (file_out) {
              if (json_out) {
                json_bounding_box["repaired"] = true;
                json_bounding_box["correct"] = false;
              } else {
                fprintf(file_out, "bounding box was repaired.\n");
              }
            }
          } else {
            if (file_out) {
              if (json_out) {
                json_bounding_box["repaired"] = false;
                json_bounding_box["correct"] = true;
              } else {
                fprintf(file_out, "bounding box is correct.\n");
              }
            }
          }
        } else {
          value = lasheader->get_x(lassummary.max.get_X());
          if (value > enlarged_max_x) {
            if (!no_warnings && file_out) {
              if (json_out) {
                char buffer[256];
                snprintf(buffer, sizeof(buffer), "WARNING: real max x larger than header max x by %lf\n", value - lasheader->max_x);
                json_bounding_box["warnings"].push_back(buffer);
              } else {
                fprintf(file_out, "WARNING: real max x larger than header max x by %lf\n", value - lasheader->max_x);
              }
            }
          }
          value = lasheader->get_x(lassummary.min.get_X());
          if (value < enlarged_min_x) {
            if (!no_warnings && file_out) {
              if (json_out) {
                char buffer[256];
                snprintf(buffer, sizeof(buffer), "WARNING: real min x smaller than header min x by %lf\n", lasheader->min_x - value);
                json_bounding_box["warnings"].push_back(buffer);
              } else {
                fprintf(file_out, "WARNING: real min x smaller than header min x by %lf\n", lasheader->min_x - value);
              }
            }
          }
          value = lasheader->get_y(lassummary.max.get_Y());
          if (value > enlarged_max_y) {
            if (!no_warnings && file_out) {
              if (json_out) {
                char buffer[256];
                snprintf(buffer, sizeof(buffer), "WARNING: real max y larger than header max y by %lf\n", value - lasheader->max_y);
                json_bounding_box["warnings"].push_back(buffer);
              } else {
                fprintf(file_out, "WARNING: real max y larger than header max y by %lf\n", value - lasheader->max_y);
              }
            }
          }
          value = lasheader->get_y(lassummary.min.get_Y());
          if (value < enlarged_min_y) {
            if (!no_warnings && file_out) {
              if (json_out) {
                char buffer[256];
                snprintf(buffer, sizeof(buffer), "WARNING: real min y smaller than header min y by %lf\n", lasheader->min_y - value);
                json_bounding_box["warnings"].push_back(buffer);
              } else {
                fprintf(file_out, "WARNING: real min y smaller than header min y by %lf\n", lasheader->min_y - value);
              }
            }
          }
          value = lasheader->get_z(lassummary.max.get_Z());
          if (value > enlarged_max_z) {
            if (!no_warnings && file_out) {
              if (json_out) {
                char buffer[256];
                snprintf(buffer, sizeof(buffer), "WARNING: real max z larger than header max z by %lf\n", value - lasheader->max_z);
                json_bounding_box["warnings"].push_back(buffer);
              } else {
                fprintf(file_out, "WARNING: real max z larger than header max z by %lf\n", value - lasheader->max_z);
              }
            }
          }
          value = lasheader->get_z(lassummary.min.get_Z());
          if (value < enlarged_min_z) {
            if (!no_warnings && file_out) {
              if (json_out) {
                char buffer[256];
                snprintf(buffer, sizeof(buffer), "WARNING: real min z smaller than header min z by %lf\n", lasheader->min_z - value);
                json_bounding_box["warnings"].push_back(buffer);
              } else {
                fprintf(file_out, "WARNING: real min z smaller than header min z by %lf\n", lasheader->min_z - value);
              }
            }
          }
        }
        if (json_out && !json_bounding_box.is_null()) json_sub_main["bounding_box"] = json_bounding_box;
      }

      if (file_out && json_out) json_main["lasinfo"].push_back(json_sub_main);

      if (file_out && (file_out != stdout) && (file_out != stderr) && !json_out) fclose(file_out);
      if (!json_out) laswriteopener.set_file_name(0);

      delete lasreader;
      if (file) fclose(file);
    }
    // When creating the JSON file, it must only be closed at the very end, otherwise an invalid json will result if there are several input files
    if (file_out && json_out) {
      std::string json_string = json_main.dump(2);
      fprintf(file_out, "%s", json_string.c_str());

      if (file_out && (file_out != stdout) && (file_out != stderr)) fclose(file_out);
      laswriteopener.set_file_name(0);
    }

    if (set_system_identifier) delete[] set_system_identifier;
    if (set_generating_software) delete[] set_generating_software;
    if (set_bounding_box) delete[] set_bounding_box;
    if (set_offset) delete[] set_offset;
    if (set_scale) delete[] set_scale;

    byebye();
  };

  void usage() override {
    fprintf(stderr, "usage:\n");
    fprintf(stderr, "lasinfo -i lidar.las\n");
    fprintf(stderr, "lasinfo -i lidar.las -compute_density -o lidar_info.txt\n");
    fprintf(stderr, "lasinfo -i *.las\n");
    fprintf(stderr, "lasinfo -i *.las -single -otxt\n");
    fprintf(stderr, "lasinfo -no_header -no_vlrs -i lidar.laz\n");
    fprintf(stderr, "lasinfo -nv -nc -stdout -i lidar.las\n");
    fprintf(stderr, "lasinfo -nv -nc -stdout -i *.laz -single | grep version\n");
    fprintf(stderr, "lasinfo -i *.laz -subseq 100000 100100 -histo user_data 8\n");
    fprintf(stderr, "lasinfo -i *.las -repair\n");
    fprintf(stderr, "lasinfo -i *.laz -repair_bb -set_file_creation 8 2007\n");
    fprintf(stderr, "lasinfo -i *.las -repair_counters -set_version 1.2\n");
    fprintf(stderr, "lasinfo -i *.laz -set_system_identifier \"hello world!\" -set_generating_software \"this is a test (-:\"\n");
  };
};

int main(int argc, char* argv[]) {
  LasTool_lasinfo lastool;
  lastool.init(argc, argv, "lasinfo");
  lastool.run();
}
