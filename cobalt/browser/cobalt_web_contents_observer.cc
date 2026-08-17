// Copyright 2025 The Cobalt Authors. All Rights Reserved.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include "cobalt/browser/cobalt_web_contents_observer.h"

#include "base/functional/bind.h"
#include "base/logging.h"
#include "base/metrics/histogram_functions.h"
#include "base/strings/utf_string_conversions.h"
#include "base/timer/timer.h"
#include "base/values.h"
#include "content/public/browser/navigation_handle.h"
#include "content/public/browser/render_frame_host.h"
#include "net/base/net_errors.h"
#if BUILDFLAG(IS_ANDROIDTV)
#include "starboard/android/shared/starboard_bridge.h"
#endif  // BUILDFLAG(IS_ANDROIDTV)
#if BUILDFLAG(IS_IOS_TVOS)
#include "cobalt/browser/tvos/network_error_handler.h"
#endif  // BUILDFLAG(IS_IOS_TVOS)

namespace cobalt {

namespace {
const int kNavigationTimeoutSeconds = 30;
#if BUILDFLAG(IS_ANDROIDTV)
const int kJniErrorTypeConnectionError = 0;

// TizenTube/YouTube TV exposes subtitle choices through its internal command
// resolver. This small injected patch makes Persian (fa) the preferred
// auto-translation language, so the user does not have to navigate the long
// Auto-translate -> Other languages list for every video.
constexpr char kPreferredSubtitleLanguageScript[] = R"JS(
(function() {
  'use strict';
  if (window.__ttPreferredSubtitleLanguageInstalled) return;
  window.__ttPreferredSubtitleLanguageInstalled = true;

  const preferredCode = 'fa';
  const preferredName = 'Persian';
  let attempts = 0;

  function install() {
    try {
      if (!window._yttv) return false;
      const holder = Object.values(window._yttv).find(
        x => x && x.instance && typeof x.instance.resolveCommand === 'function');
      if (!holder) return false;

      const resolver = holder.instance.resolveCommand;
      if (resolver.__ttPreferredSubtitleLanguage) return true;

      holder.instance.resolveCommand = function(command) {
        const popupType = command && command.openPopupAction &&
          command.openPopupAction.uniqueId;
        const result = resolver.apply(this, arguments);

        if (popupType === 'CLIENT_OVERLAY_TYPE_CAPTIONS_AUTO_TRANSLATE') {
          try {
            const items = command.openPopupAction.popup
              .overlaySectionRenderer.overlay
              .overlayTwoPanelRenderer.actionPanel
              .overlayPanelRenderer.content
              .overlayPanelItemListRenderer.items;

            const preferred = items.find(item => {
              const commands = item && item.compactLinkRenderer &&
                item.compactLinkRenderer.serviceEndpoint &&
                item.compactLinkRenderer.serviceEndpoint.commandExecutorCommand &&
                item.compactLinkRenderer.serviceEndpoint.commandExecutorCommand.commands;
              const translation = commands && commands[0] &&
                commands[0].selectSubtitlesTrackCommand &&
                commands[0].selectSubtitlesTrackCommand.translationLanguage;
              return translation &&
                (translation.languageCode === preferredCode ||
                 translation.languageName === preferredName);
            });

            const commands = preferred && preferred.compactLinkRenderer &&
              preferred.compactLinkRenderer.serviceEndpoint &&
              preferred.compactLinkRenderer.serviceEndpoint.commandExecutorCommand &&
              preferred.compactLinkRenderer.serviceEndpoint.commandExecutorCommand.commands;
            const preferredCommand = commands && commands[0];

            if (preferredCommand) {
              // Let the Auto-translate overlay open first, then execute the
              // same command that a manual Persian selection would execute.
              setTimeout(() => {
                try {
                  resolver.call(this, preferredCommand);
                  console.info('[TizenTube] Preferred subtitle language: Persian');
                } catch (e) {
                  console.warn('[TizenTube] Could not auto-select Persian', e);
                }
              }, 0);
            }
          } catch (e) {
            console.warn('[TizenTube] Preferred subtitle language patch failed', e);
          }
        }

        return result;
      };

      holder.instance.resolveCommand.__ttPreferredSubtitleLanguage = true;
      console.info('[TizenTube] Preferred auto-translate language installed: Persian (fa)');
      return true;
    } catch (e) {
      console.warn('[TizenTube] Preferred subtitle language initialization failed', e);
      return false;
    }
  }

  const timer = setInterval(() => {
    if (install() || ++attempts >= 120) clearInterval(timer);
  }, 500);
  install();
})();
)JS";
#endif  // BUILDFLAG(IS_ANDROIDTV)
}  // namespace

CobaltWebContentsObserver::CobaltWebContentsObserver(
    content::WebContents* web_contents)
    : content::WebContentsObserver(web_contents) {
  timeout_timer_ = std::make_unique<base::OneShotTimer>();
}

CobaltWebContentsObserver::~CobaltWebContentsObserver() = default;

void CobaltWebContentsObserver::SetTimerForTestInternal(
    std::unique_ptr<base::OneShotTimer> timer) {
  timeout_timer_ = std::move(timer);
}

void CobaltWebContentsObserver::DidStartNavigation(
    content::NavigationHandle* handle) {
  // M138 refinement: Ensure we don't restart timers for subframes or
  // background prerenders that haven't been activated yet.
  if (!handle->IsInPrimaryMainFrame() ||
      handle->IsServedFromBackForwardCache()) {
    LOG(INFO) << "DidStartNavigation: Skipping timer for " << handle->GetURL()
              << " (Not primary mainframe or served from BFCache)";
    return;
  }

  // Start a navigation timer with a timeout callback to raise a
  // network error dialog
  timeout_timer_->Stop();
  timeout_timer_->Start(
      FROM_HERE, base::Seconds(kNavigationTimeoutSeconds),
      base::BindOnce(&CobaltWebContentsObserver::OnNavigationTimeout,
                     weak_factory_.GetWeakPtr(), handle->GetURL().spec()));
}

// Opting for WebContentsObserver::DidFinishNavigation() over
// WebContentsObserver::PrimaryPageChanged as the network check can't
// assume HasCommitted() is true. Doing so would not catch network
// errors that are thrown before a navigation commits such as
// net::ERR_CONNECTION_TIMED_OUT and net::ERR_NAME_NOT_RESOLVED.
void CobaltWebContentsObserver::DidFinishNavigation(
    content::NavigationHandle* navigation_handle) {
  if (!navigation_handle->IsInPrimaryMainFrame()) {
    return;
  }

  timeout_timer_->Stop();
  const auto net_error_code = navigation_handle->GetNetErrorCode();
  if (net_error_code != net::OK && net_error_code != net::ERR_ABORTED) {
    base::UmaHistogramBoolean("Cobalt.WebContentsObserver.FailedNavigation",
                              true);
    base::UmaHistogramSparse("Cobalt.WebContentsObserver.FailedNavigationError",
                             -net_error_code);
    LOG(INFO) << "DidFinishNavigation: Raising platform error with code: "
              << net::ErrorToString(net_error_code);
    SetStartupDiagnosisInfo("navigation_error",
                            net::ErrorToString(net_error_code).c_str());
    RaisePlatformError(navigation_handle->GetURL().spec());
  } else if (net_error_code == net::OK) {
    base::UmaHistogramBoolean("Cobalt.WebContentsObserver.FailedNavigation",
                              false);
#if BUILDFLAG(IS_ANDROIDTV)
    platform_error_raised_count_ = 0;

    // Install the patch after the main document has committed. The injected
    // code waits for YouTube TV's internal resolver because it is initialized
    // asynchronously after navigation.
    const GURL& url = navigation_handle->GetURL();
    const std::string host = url.host();
    if (url.SchemeIsHTTPOrHTTPS() &&
        (host.find("youtube.com") != std::string::npos ||
         host.find("youtube-nocookie.com") != std::string::npos)) {
      web_contents()->GetPrimaryMainFrame()->ExecuteJavaScript(
          base::UTF8ToUTF16(kPreferredSubtitleLanguageScript),
          base::BindOnce([](base::Value) {}));
    }
#endif
  }
}

void CobaltWebContentsObserver::SetStartupDiagnosisInfo(const char* key,
                                                        const char* value) {
#if BUILDFLAG(IS_ANDROID)
  starboard::StarboardBridge::GetInstance()->SetStartupDiagnosisInfo(key,
                                                                     value);
#endif
}

void CobaltWebContentsObserver::OnNavigationTimeout(const std::string& url) {
  base::UmaHistogramBoolean("Cobalt.Network.NavigationTimeout", true);
  RaisePlatformError(url);
}

void CobaltWebContentsObserver::RaisePlatformError(const std::string& url) {
#if BUILDFLAG(IS_ANDROIDTV)
  JNIEnv* env = base::android::AttachCurrentThread();
  auto* starboard_bridge = starboard::StarboardBridge::GetInstance();

  // Don't raise a new platform error if one is already showing
  if (starboard_bridge->IsPlatformErrorShowing(env)) {
    return;
  }
  platform_error_raised_count_++;
  base::UmaHistogramCounts100("Cobalt.Network.PlatformErrorCount",
                              platform_error_raised_count_);
  starboard_bridge->RaisePlatformError(env, kJniErrorTypeConnectionError, 0,
                                       url);
#elif BUILDFLAG(IS_IOS_TVOS)
  ShowPlatformErrorDialog(web_contents());
#else
  NOTIMPLEMENTED();
#endif  // BUILDFLAG(IS_ANDROIDTV)
}

}  // namespace cobalt
