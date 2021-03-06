// Copyright (c) 2012 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "chrome/browser/chrome_browser_main_linux.h"

#include "chrome/browser/media_gallery/media_device_notifications_linux.h"

#if defined(USE_LINUX_BREAKPAD)
#include <stdlib.h>

#include "base/linux_util.h"
#include "chrome/app/breakpad_linuxish.h"
#include "chrome/browser/prefs/pref_service.h"
#include "chrome/common/env_vars.h"
#include "chrome/common/pref_names.h"
#include "content/public/browser/browser_thread.h"

#if defined(OS_CHROMEOS)
#include "chrome/browser/chromeos/cros_settings.h"
#include "chrome/browser/chromeos/cros_settings_names.h"
#include "chrome/common/chrome_switches.h"
#include "chrome/common/chrome_version_info.h"
#endif

#endif  // defined(USE_LINUX_BREAKPAD)

namespace {

#if defined(USE_LINUX_BREAKPAD)
void GetLinuxDistroCallback() {
  base::GetLinuxDistro();  // Initialize base::linux_distro if needed.
}

bool IsCrashReportingEnabled(const PrefService* local_state) {
  // Check whether we should initialize the crash reporter. It may be disabled
  // through configuration policy or user preference. It must be disabled for
  // Guest mode on Chrome OS in Stable channel.
  // The kHeadless environment variable overrides the decision, but only if the
  // crash service is under control of the user. It is used by QA testing
  // infrastructure to switch on generation of crash reports.
#if defined(OS_CHROMEOS)
  bool is_guest_session =
      CommandLine::ForCurrentProcess()->HasSwitch(switches::kGuestSession);
  bool is_stable_channel =
      chrome::VersionInfo::GetChannel() == chrome::VersionInfo::CHANNEL_STABLE;
  // TODO(pastarmovj): Consider the TrustedGet here.
  bool reporting_enabled;
  chromeos::CrosSettings::Get()->GetBoolean(chromeos::kStatsReportingPref,
                                            &reporting_enabled);
  bool breakpad_enabled =
      !(is_guest_session && is_stable_channel) && reporting_enabled;
  if (!breakpad_enabled)
    breakpad_enabled = getenv(env_vars::kHeadless) != NULL;
#else
  const PrefService::Preference* metrics_reporting_enabled =
      local_state->FindPreference(prefs::kMetricsReportingEnabled);
  CHECK(metrics_reporting_enabled);
  bool breakpad_enabled =
      local_state->GetBoolean(prefs::kMetricsReportingEnabled);
  if (!breakpad_enabled && metrics_reporting_enabled->IsUserModifiable())
    breakpad_enabled = getenv(env_vars::kHeadless) != NULL;
#endif  // defined(OS_CHROMEOS)
  return breakpad_enabled;
}
#endif  // defined(USE_LINUX_BREAKPAD)

}  // namespace

ChromeBrowserMainPartsLinux::ChromeBrowserMainPartsLinux(
    const content::MainFunctionParams& parameters)
    : ChromeBrowserMainPartsPosix(parameters) {
}

ChromeBrowserMainPartsLinux::~ChromeBrowserMainPartsLinux() {
}

void ChromeBrowserMainPartsLinux::PreProfileInit() {
#if defined(USE_LINUX_BREAKPAD)
  // Needs to be called after we have chrome::DIR_USER_DATA and
  // g_browser_process.  This happens in PreCreateThreads.
  content::BrowserThread::PostTask(content::BrowserThread::FILE,
                                   FROM_HERE,
                                   base::Bind(&GetLinuxDistroCallback));

  if (IsCrashReportingEnabled(local_state()))
    InitCrashReporter();
#endif

  const FilePath kDefaultMtabPath("/etc/mtab");
  media_device_notifications_linux_ =
      new chrome::MediaDeviceNotificationsLinux(kDefaultMtabPath);
  media_device_notifications_linux_->Init();

  ChromeBrowserMainPartsPosix::PreProfileInit();
}
