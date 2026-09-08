/**
 * @file globals.cpp
 * @brief Definitions for globally accessible variables and functions.
 */
#include "globals.h"

safe::mail_t mail::man;
thread_pool_util::ThreadPool task_pool;
bool display_cursor = true;

// VDD/Zako globals are referenced by shared display_device code on every
// platform; only the nvprefs singleton is Windows-specific.
const std::string VDD_NAME = "ZakoHDR";
const std::string ZAKO_NAME = "Zako HDR";
std::string zako_device_id;
bool is_running_as_system_user = false;

#ifdef _WIN32
nvprefs::nvprefs_interface nvprefs_instance;
#endif
