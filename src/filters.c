#include "filters.h"

void reverse_chars(char *str, char *rv_str, int size)
{
  for (int i = 0; i < size; i++)
  {
    rv_str[i] = str[size - 1 - i];
  }
  for (int i = size; i < INPUT_BUFFER_SIZE; i++)
  {
    rv_str[i] = '\0';
  }
}

time_t parse_iso8601(const char *datetime)
{
  struct tm tm = {0};
  int year, month, day, hour, minute, second;

  if (sscanf(datetime, "%d-%d-%dT%d:%d:%dZ",
             &year, &month, &day, &hour, &minute, &second) != 6)
  {
    fprintf(stderr, "Invalid ISO 8601 format\n");
    return (time_t)-1;
  }

  tm.tm_year = year - 1900; // tm_year is years since 1900
  tm.tm_mon = month - 1;    // tm_mon is months since January [0-11]
  tm.tm_mday = day;
  tm.tm_hour = hour;
  tm.tm_min = minute;
  tm.tm_sec = second;

  // Convert to UTC time_t
#if defined(_WIN32) || defined(_WIN64)
  return _mkgmtime(&tm); // Windows equivalent of timegm
#else
  return timegm(&tm); // Linux/macOS
#endif
}

/**
 * Parses a European date in format: DD.MM.YYYY
 * Example: 02.10.2020
 */
time_t parse_european_date(const char *date)
{
  struct tm tm = {0};
  int day, month, year;

  if (sscanf(date, "%d.%d.%d", &day, &month, &year) != 3)
  {
    fprintf(stderr, "Invalid European date format\n");
    return (time_t)-1;
  }

  tm.tm_year = year - 1900;
  tm.tm_mon = month - 1;
  tm.tm_mday = day;
  tm.tm_hour = 0;
  tm.tm_min = 0;
  tm.tm_sec = 0;

  // Convert to local time_t
  return mktime(&tm);
}

void apply_filter_values(GpxCollection *c)
{
  for (int i = 0; i < c->total_tracks; i++)
  {

    // default: visible
    c->tracks[i].visible_in_list = true;

    // check type
    if(c->tracks[i].act_type == Run && c->filters.showRuns == false)
      c->tracks[i].visible_in_list = false;
    if(c->tracks[i].act_type == Cycling && c->filters.showCycling == false)
      c->tracks[i].visible_in_list = false;
    if(c->tracks[i].act_type == Hike && c->filters.showHikes == false)
      c->tracks[i].visible_in_list = false;
    if(c->tracks[i].act_type == Other && c->filters.showOther == false)
      c->tracks[i].visible_in_list = false;

    // check limits
    if (c->tracks[i].distance < c->filters.distance_low || c->tracks[i].distance > c->filters.distance_high)
      c->tracks[i].visible_in_list = false;
    if (c->tracks[i].duration_secs < c->filters.duration_secs_low || c->tracks[i].duration_secs > c->filters.duration_secs_high)
      c->tracks[i].visible_in_list = false;
    if (c->tracks[i].secs_per_km < c->filters.secs_per_km_low || c->tracks[i].secs_per_km > c->filters.secs_per_km_high)
      c->tracks[i].visible_in_list = false;
    if ((c->tracks[i].elev_up - c->filters.elev_up_low ) < -0.01 || c->tracks[i].elev_up - c->filters.elev_up_high > 0.01)
      c->tracks[i].visible_in_list = false;
    if (c->tracks[i].elev_down - c->filters.elev_down_low < -0.01 || c->tracks[i].elev_down - c->filters.elev_down_high > 0.01)
      c->tracks[i].visible_in_list = false;
    if (c->tracks[i].high_point - c->filters.high_point_low < -0.01 || c->tracks[i].high_point - c->filters.high_point_high > 0.01)
      c->tracks[i].visible_in_list = false;
    if (parse_iso8601(c->tracks[i].start_time_raw) < parse_european_date(c->filters.start_date_str_filter) || parse_iso8601(c->tracks[i].end_time_raw) > parse_european_date(c->filters.end_date_str_filter))
      c->tracks[i].visible_in_list = false;
  }
  int counter = 0;
  for (int i = 0; i < c->total_tracks; i++)
  {
    if (c->tracks[i].visible_in_list)
      counter++;
  }
  sprintf(c->total_visible_tracks_str, "Shown: %d of %d Tracks", counter, c->total_tracks);
}

int digit(char c)
{
  return (c == '\0') ? 0 : (int)c - (int)'0';
}

float duration_str_to_duration_float(char *str)
{
  char rv_str[INPUT_BUFFER_SIZE];
  reverse_chars(str, rv_str, strlen(str));
  return digit(rv_str[0]) +
         10 * digit(rv_str[1]) +
         60 * digit(rv_str[3]) +
         600 * digit(rv_str[4]) +
         60 * 60 * digit(rv_str[6]) +
         60 * 600 * digit(rv_str[7]) +
         60 * 6000 * digit(rv_str[8]);
}

float pace_str_to_pace_float(char *str)
{
  char rv_str[INPUT_BUFFER_SIZE];
  reverse_chars(str, rv_str, strlen(str));
  for (int i = 0; i < INPUT_BUFFER_SIZE; i++)
  {
    printf("rv[%d]; %c\n", i, rv_str[i]);
  }
  return digit(rv_str[0]) +
         10 * digit(rv_str[1]) +
         60 * digit(rv_str[3]) +
         10 * 60 * digit(rv_str[4]) +
         100 * 60 * digit(rv_str[5]) +
         1000 * 60 * digit(rv_str[6]);
}

void save_filter_values(FilterSettings *filter)
{
  // date
  if (filter->start_date_str[0] != '\0')
    sprintf(filter->start_date_str_filter, filter->start_date_str);
  else
    sprintf(filter->start_date_str_filter, "01.01.1980\0");


  if (filter->end_date_str[0] != '\0')
    sprintf(filter->start_date_str_filter, filter->start_date_str);
  else
    sprintf(filter->end_date_str_filter, "01.01.9000\0");

  // uphill
  if (filter->elev_up_high_str[0] != '\0')
    filter->elev_up_high = (float)atof(filter->elev_up_high_str);
  else
    filter->elev_up_high = FLT_MAX;

  if (filter->elev_up_low_str[0] != '\0')
    filter->elev_up_low = (float)atof(filter->elev_up_low_str);
  else
    filter->elev_up_low = FLT_MIN;

  // downhill
  if (filter->elev_down_high_str[0] != '\0')
    filter->elev_down_high = (float)atof(filter->elev_down_high_str);
  else
    filter->elev_down_high = FLT_MAX;

  if (filter->elev_down_low_str[0] != '\0')
    filter->elev_down_low = (float)atof(filter->elev_down_low_str);
  else
    filter->elev_down_low = FLT_MIN;

  // high point
  if (filter->high_point_high_str[0] != '\0')
    filter->high_point_high = (float)atof(filter->high_point_high_str);
  else
    filter->high_point_high = FLT_MAX;

  if (filter->high_point_low_str[0] != '\0')
    filter->high_point_low = (float)atof(filter->high_point_low_str);
  else
    filter->high_point_low = FLT_MIN;

  // distance
  if (filter->distance_high_str[0] != '\0')
    filter->distance_high = (float)atof(filter->distance_high_str);
  else
    filter->distance_high = FLT_MAX;

  if (filter->distance_low_str[0] != '\0')
    filter->distance_low = (float)atof(filter->distance_low_str);
  else
    filter->distance_low = FLT_MIN;

  // duration
  if (filter->duration_high_str[0] != '\0')
    filter->duration_secs_high = duration_str_to_duration_float(filter->duration_high_str);
  else
    filter->duration_secs_high = FLT_MAX;

  if (filter->duration_low_str[0] != '\0')
    filter->duration_secs_low = duration_str_to_duration_float(filter->duration_low_str);
  else
    filter->duration_secs_low = FLT_MIN;

  // pace
  if (filter->pace_high_str[0] != '\0')
    filter->secs_per_km_high = pace_str_to_pace_float(filter->pace_high_str);
  else
    filter->secs_per_km_high = FLT_MAX;

  if (filter->pace_low_str[0] != '\0')
  {
    filter->secs_per_km_low = pace_str_to_pace_float(filter->pace_low_str);
    printf("min pace: %f s\n", filter->secs_per_km_low);
  }
  else
    filter->secs_per_km_low = FLT_MIN;
}

void reset_filters(FilterSettings *filter)
{
  filter->distance_high_str[0] = '\0';
  filter->distance_low_str[0] = '\0';
  filter->duration_high_str[0] = '\0';
  filter->duration_low_str[0] = '\0';
  filter->pace_high_str[0] = '\0';
  filter->pace_low_str[0] = '\0';
  filter->elev_up_high_str[0] = '\0';
  filter->elev_up_low_str[0] = '\0';
  filter->elev_down_high_str[0] = '\0';
  filter->elev_down_low_str[0] = '\0';
  filter->start_date_str[0] = '\0';
  filter->end_date_str[0] = '\0';
  filter->high_point_high_str[0] = '\0';
  filter->high_point_low_str[0] = '\0';
  filter->showCycling = true;
  filter->showOther = true;
  filter->showRuns = true;
  filter->showHikes = true;
  save_filter_values(filter);
}