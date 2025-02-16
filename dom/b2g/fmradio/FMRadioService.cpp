/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* vim: set ts=8 sts=2 et sw=2 tw=80: */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this file,
 * You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "FMRadioService.h"
#include "mozilla/ClearOnShutdown.h"
#include "mozilla/DebugOnly.h"
#include "mozilla/dom/FMRadioChild.h"
#include "mozilla/dom/power/PowerManagerService.h"
#include "mozilla/Hal.h"
#include "mozilla/Preferences.h"
#include "mozilla/Services.h"
#include "mozilla/dom/WakeLock.h"
#include "nsContentUtils.h"
#include "nsIAudioManager.h"
#include "nsJSUtils.h"
#include "nsServiceManagerUtils.h"
#include "nsXULAppAPI.h"

#undef LOG
#define LOG(args...) FM_LOG("FMRadio", args)

#define TUNE_THREAD_TIMEOUT_MS 5000

#define BAND_87500_108000_kHz 1
#define BAND_76000_108000_kHz 2
#define BAND_76000_90000_kHz 3

#define SETTING_KEY_AIRPLANEMODE_ENABLED u"airplaneMode.enabled"_ns

#define DOM_PARSED_RDS_GROUPS ((0x2 << 30) | (0x3 << 4) | (0x3 << 0))

using mozilla::Preferences;

BEGIN_FMRADIO_NAMESPACE

#if defined(PRODUCT_MANUFACTURER_SPRD) || defined(PRODUCT_MANUFACTURER_MTK)
class AntennaSwitchObserver : public hal::SwitchObserver {
 public:
  void Notify(const hal::SwitchEvent& aEvent) {
    if ((hal::IsFMRadioOn()) && (aEvent.device() == hal::SWITCH_HEADPHONES)) {
      hal::SetFMRadioAntenna(aEvent.status());
    }
  }
};
#endif

// static
IFMRadioService* IFMRadioService::Singleton() {
  if (!XRE_IsParentProcess()) {
    return FMRadioChild::Singleton();
  } else {
    return FMRadioService::Singleton();
  }
}

StaticRefPtr<FMRadioService> FMRadioService::sFMRadioService;

FMRadioService::FMRadioService()
    : mPendingFrequencyInKHz(0),
      mState(Disabled),
      mHasReadAirplaneModeSetting(false),
      mAirplaneModeEnabled(false),
      mRDSEnabled(false),
      mPendingRequest(nullptr),
      mObserverList(FMRadioEventObserverList()),
#if defined(PRODUCT_MANUFACTURER_SPRD) || defined(PRODUCT_MANUFACTURER_MTK)
      mObserver(new AntennaSwitchObserver()),
#endif
      mRDSGroupMask(0),
      mLastPI(0),
      mPI(0),
      mPTY(0),
      mPISet(false),
      mPTYSet(false),
      mRDSLock("FMRadioService::mRDSLock"),
      mPSNameState(0),
      mRadiotextAB(false),
      mRDSGroupSet(false),
      mPSNameSet(false),
      mRadiotextSet(false) {
  memset(mPSName, 0, sizeof(mPSName));
  memset(mRadiotext, 0, sizeof(mRadiotext));
  memset(mTempPSName, 0, sizeof(mTempPSName));
  memset(mTempRadiotext, 0, sizeof(mTempRadiotext));

  // Read power state and frequency from Hal.
  mEnabled = hal::IsFMRadioOn();
  if (mEnabled) {
    mPendingFrequencyInKHz = hal::GetFMRadioFrequency();
    SetState(Enabled);
  }

  // Read the pref which is set by device config.
  mRequireWakeLock = Preferences::GetBool("device.fm.requirewakelock", false);

  switch (Preferences::GetInt("dom.fmradio.band", BAND_87500_108000_kHz)) {
    case BAND_76000_90000_kHz:
      mUpperBoundInKHz = 90000;
      mLowerBoundInKHz = 76000;
      break;
    case BAND_76000_108000_kHz:
      mUpperBoundInKHz = 108000;
      mLowerBoundInKHz = 76000;
      break;
    case BAND_87500_108000_kHz:
    default:
      mUpperBoundInKHz = 108000;
      mLowerBoundInKHz = 87500;
      break;
  }

  mChannelWidthInKHz = Preferences::GetInt("dom.fmradio.channelWidth", 100);
  switch (mChannelWidthInKHz) {
    case 50:
    case 100:
    case 200:
      break;
    default:
      NS_WARNING("Invalid channel width specified in dom.fmradio.channelwidth");
      mChannelWidthInKHz = 100;
      break;
  }

  mPreemphasis = Preferences::GetInt("dom.fmradio.preemphasis", 50);
  switch (mPreemphasis) {
    // values in microseconds
    case 0:
    case 50:
    case 75:
      break;
    default:
      NS_WARNING("Invalid preemphasis specified in dom.fmradio.preemphasis");
      mPreemphasis = 50;
      break;
  }

  nsCOMPtr<nsISettingsManager> settings =
      do_GetService("@mozilla.org/sidl-native/settings;1");
  if (settings) {
    settings->Get(SETTING_KEY_AIRPLANEMODE_ENABLED, this);
    settings->AddObserver(SETTING_KEY_AIRPLANEMODE_ENABLED, this, this);
  } else {
    LOG("Failed to get SettingsManager");
  }

  hal::RegisterFMRadioObserver(this);
  hal::RegisterFMRadioRDSObserver(this);
#if defined(PRODUCT_MANUFACTURER_SPRD) || defined(PRODUCT_MANUFACTURER_MTK)
  hal::RegisterSwitchObserver(hal::SWITCH_HEADPHONES, mObserver.get());
#endif
}

FMRadioService::~FMRadioService() {
  nsCOMPtr<nsISettingsManager> settings =
      do_GetService("@mozilla.org/sidl-native/settings;1");
  if (settings) {
    settings->RemoveObserver(SETTING_KEY_AIRPLANEMODE_ENABLED, this, this);
  } else {
    LOG("Failed to get SettingsManager");
  }

#if defined(PRODUCT_MANUFACTURER_SPRD) || defined(PRODUCT_MANUFACTURER_MTK)
  hal::UnregisterSwitchObserver(hal::SWITCH_HEADPHONES, mObserver.get());
#endif
  hal::UnregisterFMRadioRDSObserver(this);
  hal::UnregisterFMRadioObserver(this);
}

void FMRadioService::EnableFMRadio() {
  hal::FMRadioSettings info;
  info.upperLimit() = mUpperBoundInKHz;
  info.lowerLimit() = mLowerBoundInKHz;
  info.spaceType() = mChannelWidthInKHz;
  info.preEmphasis() = mPreemphasis;

  hal::EnableFMRadio(info);

  if (!mTuneThread) {
    // hal::FMRadioSeek and hal::SetFMRadioFrequency run on this thread. These
    // call ioctls that can stall the main thread, so we run them here.
    mTuneThread = new LazyIdleThread(TUNE_THREAD_TIMEOUT_MS, "FM Tuning");
  }
}

nsresult FMRadioService::SettingCallback(nsISettingInfo* aInfo) {
  MOZ_ASSERT(NS_IsMainThread());
  MOZ_ASSERT(aInfo);

  nsString name, value;
  aInfo->GetName(name);
  aInfo->GetValue(value);
  if (name.Equals(SETTING_KEY_AIRPLANEMODE_ENABLED)) {
    mHasReadAirplaneModeSetting = true;
    mAirplaneModeEnabled = value.EqualsLiteral("true");

    // Disable FM radio HW if Airplane mode is enabled.
    if (mAirplaneModeEnabled) {
      Disable(nullptr);
    }
  }
  return NS_OK;
}

// Implements nsISettingsGetResponse::Resolve()
NS_IMETHODIMP FMRadioService::Resolve(nsISettingInfo* aInfo) {
  return SettingCallback(aInfo);
}

// Implements nsISettingsGetResponse::Reject()
NS_IMETHODIMP FMRadioService::Reject(nsISettingError* aError) { return NS_OK; }

// Implements nsISettingsObserver::ObserveSetting()
NS_IMETHODIMP FMRadioService::ObserveSetting(nsISettingInfo* aInfo) {
  return SettingCallback(aInfo);
}

// Implements nsISidlDefaultResponse
NS_IMETHODIMP FMRadioService::Resolve() { return NS_OK; }
NS_IMETHODIMP FMRadioService::Reject() { return NS_OK; }

void FMRadioService::DisableFMRadio() {
  // Due to an unknown reason, if we call mTuneThread->Shutdown() here it may
  // cause crashes in LazyIdleThread::ShutdownThread(). Luckily we don't really
  // need to shut down mTuneThread each time FM radio is disabled, because
  // LazyIdleThread always shuts down its internal thread after the specified
  // idle time, and by default it can automatically shuts itself down when
  // observing "xpcom-shutdown-threads". Therefore it should be fine to keep
  // mTuneThread alive until FMRadioService is destructed. See Bug 123131.

  // Fix Bug 796733. DisableFMRadio should be called before
  // SetFmRadioAudioEnabled to prevent the annoying beep sound.
  hal::DisableFMRadio();
  EnableAudio(false);
};

void FMRadioService::DispatchFMRadioEventToMainThread(
    enum FMRadioEventType aType) {
  RefPtr<FMRadioService> self = this;
  NS_DispatchToMainThread(NS_NewRunnableFunction(
      "FMRadioService::DispatchFMRadioEventToMainThread",
      [self, aType]() -> void { self->NotifyFMRadioEvent(aType); }));
}

void FMRadioService::TransitionState(const FMRadioResponseType& aResponse,
                                     FMRadioState aState) {
  if (mPendingRequest) {
    mPendingRequest->SetReply(aResponse);
    NS_DispatchToMainThread(mPendingRequest);
  }

  SetState(aState);
}

void FMRadioService::SetState(FMRadioState aState) {
  mState = aState;
  mPendingRequest = nullptr;
}

void FMRadioService::AddObserver(FMRadioEventObserver* aObserver) {
  MOZ_ASSERT(NS_IsMainThread(), "Wrong thread!");
  mObserverList.AddObserver(aObserver);
}

void FMRadioService::RemoveObserver(FMRadioEventObserver* aObserver) {
  MOZ_ASSERT(NS_IsMainThread(), "Wrong thread!");
  mObserverList.RemoveObserver(aObserver);

  if (mObserverList.Length() == 0) {
    // Turning off the FM radio HW because observer list is empty.
    if (hal::IsFMRadioOn()) {
      DoDisable();
    }
  }
}

void FMRadioService::EnableAudio(bool aAudioEnabled) {
  MOZ_ASSERT(NS_IsMainThread(), "Wrong thread!");

  nsCOMPtr<nsIAudioManager> audioManager =
      do_GetService("@mozilla.org/telephony/audiomanager;1");
  if (!audioManager) {
    return;
  }

  bool audioEnabled;
  audioManager->GetFmRadioAudioEnabled(&audioEnabled);
  if (audioEnabled != aAudioEnabled) {
    audioManager->SetFmRadioAudioEnabled(aAudioEnabled);
  }
}

void FMRadioService::SetVolume(float aVolume) {
  MOZ_ASSERT(NS_IsMainThread(), "Wrong thread!");

  nsCOMPtr<nsIAudioManager> audioManager =
      do_GetService("@mozilla.org/telephony/audiomanager;1");
  if (!audioManager) {
    return;
  }

  audioManager->SetFmRadioContentVolume(aVolume);
}

/**
 * Round the frequency to match the range of frequency and the channel width. If
 * the given frequency is out of range, return 0. For example:
 *  - lower: 87500KHz, upper: 108000KHz, channel width: 200KHz
 *    87.6MHz is rounded to 87700KHz
 *    87.58MHz is rounded to 87500KHz
 *    87.49MHz is rounded to 87500KHz
 *    109MHz is not rounded, 0 will be returned
 *
 * We take frequency in MHz to prevent precision losing, and return rounded
 * value in KHz for Gonk using.
 */
int32_t FMRadioService::RoundFrequency(double aFrequencyInMHz) {
  double halfChannelWidthInMHz = mChannelWidthInKHz / 1000.0 / 2;

  // Make sure 87.49999MHz would be rounded to the lower bound when
  // the lower bound is 87500KHz.
  if (aFrequencyInMHz < mLowerBoundInKHz / 1000.0 - halfChannelWidthInMHz ||
      aFrequencyInMHz > mUpperBoundInKHz / 1000.0 + halfChannelWidthInMHz) {
    return 0;
  }

  int32_t partToBeRounded = round(aFrequencyInMHz * 1000) - mLowerBoundInKHz;
  int32_t roundedPart =
      round(partToBeRounded / (double)mChannelWidthInKHz) * mChannelWidthInKHz;

  return mLowerBoundInKHz + roundedPart;
}

bool FMRadioService::IsEnabled() const {
  MOZ_ASSERT(NS_IsMainThread(), "Wrong thread!");
  return hal::IsFMRadioOn();
}

bool FMRadioService::IsRDSEnabled() const {
  MOZ_ASSERT(NS_IsMainThread(), "Wrong thread!");
  return mRDSEnabled;
}

double FMRadioService::GetFrequency() const {
  MOZ_ASSERT(NS_IsMainThread(), "Wrong thread!");
  if (IsEnabled()) {
    int32_t frequencyInKHz = hal::GetFMRadioFrequency();
    return frequencyInKHz / 1000.0;
  }

  return 0;
}

double FMRadioService::GetFrequencyUpperBound() const {
  MOZ_ASSERT(NS_IsMainThread(), "Wrong thread!");
  return mUpperBoundInKHz / 1000.0;
}

double FMRadioService::GetFrequencyLowerBound() const {
  MOZ_ASSERT(NS_IsMainThread(), "Wrong thread!");
  return mLowerBoundInKHz / 1000.0;
}

double FMRadioService::GetChannelWidth() const {
  MOZ_ASSERT(NS_IsMainThread(), "Wrong thread!");
  return mChannelWidthInKHz / 1000.0;
}

Nullable<unsigned short> FMRadioService::GetPi() const {
  MOZ_ASSERT(NS_IsMainThread(), "Wrong thread!");
  if (!mPISet) {
    return Nullable<unsigned short>();
  }
  return Nullable<unsigned short>(mPI);
}

Nullable<uint8_t> FMRadioService::GetPty() const {
  MOZ_ASSERT(NS_IsMainThread(), "Wrong thread!");
  if (!mPTYSet) {
    return Nullable<uint8_t>();
  }
  return Nullable<uint8_t>(mPTY);
}

bool FMRadioService::GetPs(nsString& aPSName) {
  MutexAutoLock lock(mRDSLock);
  if (mPSNameSet) {
    aPSName = nsString(mPSName);
  }
  return mPSNameSet;
}

bool FMRadioService::GetRt(nsString& aRadiotext) {
  MutexAutoLock lock(mRDSLock);
  if (mRadiotextSet) {
    aRadiotext = nsString(mRadiotext);
  }
  return mRadiotextSet;
}

bool FMRadioService::GetRdsgroup(uint64_t& aRDSGroup) {
  MutexAutoLock lock(mRDSLock);
  aRDSGroup = mRDSGroup;
  return mRDSGroupSet;
}

void FMRadioService::Enable(double aFrequencyInMHz,
                            FMRadioReplyRunnable* aReplyRunnable) {
  MOZ_ASSERT(NS_IsMainThread(), "Wrong thread!");
  MOZ_ASSERT(aReplyRunnable);

  switch (mState) {
    case Seeking:
    case Enabled:
      aReplyRunnable->SetReply(ErrorResponse(u"FM radio currently enabled"_ns));
      NS_DispatchToMainThread(aReplyRunnable);
      return;
    case Disabling:
      aReplyRunnable->SetReply(
          ErrorResponse(u"FM radio currently disabling"_ns));
      NS_DispatchToMainThread(aReplyRunnable);
      return;
    case Enabling:
      aReplyRunnable->SetReply(
          ErrorResponse(u"FM radio currently enabling"_ns));
      NS_DispatchToMainThread(aReplyRunnable);
      return;
    case Disabled:
      break;
  }

  int32_t roundedFrequency = RoundFrequency(aFrequencyInMHz);

  if (!roundedFrequency) {
    aReplyRunnable->SetReply(ErrorResponse(u"Frequency is out of range"_ns));
    NS_DispatchToMainThread(aReplyRunnable);
    return;
  }

  if (mHasReadAirplaneModeSetting && mAirplaneModeEnabled) {
    aReplyRunnable->SetReply(
        ErrorResponse(u"Airplane mode currently enabled"_ns));
    NS_DispatchToMainThread(aReplyRunnable);
    return;
  }

  SetState(Enabling);
  // Cache the enable request just in case disable() is called
  // while the FM radio HW is being enabled.
  mPendingRequest = aReplyRunnable;

  // Cache the frequency value, and set it after the FM radio HW is enabled
  mPendingFrequencyInKHz = roundedFrequency;

  RefPtr<FMRadioService> self = this;
  NS_DispatchToMainThread(NS_NewRunnableFunction(
      "FMRadioService::Enable", [self]() -> void { self->EnableFMRadio(); }));
}

void FMRadioService::Disable(FMRadioReplyRunnable* aReplyRunnable) {
  // When airplane-mode is enabled, we will call this function from
  // FMRadioService::Observe without passing a FMRadioReplyRunnable,
  // so we have to check if |aReplyRunnable| is null before we dispatch it.
  MOZ_ASSERT(NS_IsMainThread(), "Wrong thread!");

  switch (mState) {
    case Disabling:
      if (aReplyRunnable) {
        aReplyRunnable->SetReply(
            ErrorResponse(u"FM radio currently disabling"_ns));
        NS_DispatchToMainThread(aReplyRunnable);
      }
      return;
    case Disabled:
      if (aReplyRunnable) {
        aReplyRunnable->SetReply(
            ErrorResponse(u"FM radio currently disabled"_ns));
        NS_DispatchToMainThread(aReplyRunnable);
      }
      return;
    case Enabled:
    case Enabling:
    case Seeking:
      break;
  }

  RefPtr<FMRadioReplyRunnable> enablingRequest = mPendingRequest;

  // If the FM Radio is currently seeking, no fail-to-seek or similar
  // event will be fired, execute the seek callback manually.
  if (mState == Seeking) {
    TransitionState(ErrorResponse(u"Seek action is cancelled"_ns), Disabling);
  }

  FMRadioState preState = mState;
  SetState(Disabling);
  mPendingRequest = aReplyRunnable;

  if (preState == Enabling) {
    // If the radio is currently enabling, we fire the error callback on the
    // enable request immediately. When the radio finishes enabling, we'll call
    // DoDisable and fire the success callback on the disable request.
    enablingRequest->SetReply(ErrorResponse(u"Enable action is cancelled"_ns));
    NS_DispatchToMainThread(enablingRequest);

    // If we haven't read the airplane mode settings yet we won't enable the
    // FM radio HW, so fail the disable request immediately.
    if (!mHasReadAirplaneModeSetting) {
      SetState(Disabled);

      if (aReplyRunnable) {
        aReplyRunnable->SetReply(SuccessResponse());
        NS_DispatchToMainThread(aReplyRunnable);
      }
    }

    return;
  }

  DoDisable();
}

void FMRadioService::DoDisable() {
  // To make such codes work:
  //    navigator.mozFMRadio.disable();
  //    navigator.mozFMRadio.ondisabled = function() {
  //      console.log("We will catch disabled event ");
  //    };
  // we need to call hal::DisableFMRadio() asynchronously. Same reason for
  // EnableFMRadio and hal::SetFMRadioFrequency.
  RefPtr<FMRadioService> self = this;
  NS_DispatchToMainThread(
      NS_NewRunnableFunction("FMRadioService::DoDisable",
                             [self]() -> void { self->DisableFMRadio(); }));
}

void FMRadioService::SetFrequency(double aFrequencyInMHz,
                                  FMRadioReplyRunnable* aReplyRunnable) {
  MOZ_ASSERT(NS_IsMainThread(), "Wrong thread!");
  MOZ_ASSERT(aReplyRunnable);

  switch (mState) {
    case Disabled:
      aReplyRunnable->SetReply(
          ErrorResponse(u"FM radio currently disabled"_ns));
      NS_DispatchToMainThread(aReplyRunnable);
      return;
    case Enabling:
      aReplyRunnable->SetReply(
          ErrorResponse(u"FM radio currently enabling"_ns));
      NS_DispatchToMainThread(aReplyRunnable);
      return;
    case Disabling:
      aReplyRunnable->SetReply(
          ErrorResponse(u"FM radio currently disabling"_ns));
      NS_DispatchToMainThread(aReplyRunnable);
      return;
    case Seeking:
      hal::CancelFMRadioSeek();
      TransitionState(ErrorResponse(u"Seek action is cancelled"_ns), Enabled);
      break;
    case Enabled:
      break;
  }

  int32_t roundedFrequency = RoundFrequency(aFrequencyInMHz);

  if (!roundedFrequency) {
    aReplyRunnable->SetReply(ErrorResponse(u"Frequency is out of range"_ns));
    NS_DispatchToMainThread(aReplyRunnable);
    return;
  }

  mTuneThread->Dispatch(
      NS_NewRunnableFunction("FMRadioService::SetFrequency",
                             [roundedFrequency]() -> void {
                               hal::SetFMRadioFrequency(roundedFrequency);
                             }),
      nsIThread::DISPATCH_NORMAL);

  aReplyRunnable->SetReply(SuccessResponse());
  NS_DispatchToMainThread(aReplyRunnable);
}

void FMRadioService::Seek(hal::FMRadioSeekDirection aDirection,
                          FMRadioReplyRunnable* aReplyRunnable) {
  MOZ_ASSERT(NS_IsMainThread(), "Wrong thread!");
  MOZ_ASSERT(aReplyRunnable);

  switch (mState) {
    case Enabling:
      aReplyRunnable->SetReply(
          ErrorResponse(u"FM radio currently enabling"_ns));
      NS_DispatchToMainThread(aReplyRunnable);
      return;
    case Disabled:
      aReplyRunnable->SetReply(
          ErrorResponse(u"FM radio currently disabled"_ns));
      NS_DispatchToMainThread(aReplyRunnable);
      return;
    case Seeking:
      aReplyRunnable->SetReply(ErrorResponse(u"FM radio currently seeking"_ns));
      NS_DispatchToMainThread(aReplyRunnable);
      return;
    case Disabling:
      aReplyRunnable->SetReply(
          ErrorResponse(u"FM radio currently disabling"_ns));
      NS_DispatchToMainThread(aReplyRunnable);
      return;
    case Enabled:
      break;
  }

  SetState(Seeking);
  mPendingRequest = aReplyRunnable;

  mTuneThread->Dispatch(
      NS_NewRunnableFunction("FMRadioService::Seek",
                             [aDirection]() -> void {
                               switch (aDirection) {
                                 case hal::FM_RADIO_SEEK_DIRECTION_UP:
                                 case hal::FM_RADIO_SEEK_DIRECTION_DOWN:
                                   hal::FMRadioSeek(aDirection);
                                   break;
                                 default:
                                   MOZ_CRASH();
                               }
                             }),
      nsIThread::DISPATCH_NORMAL);
}

void FMRadioService::CancelSeek(FMRadioReplyRunnable* aReplyRunnable) {
  MOZ_ASSERT(NS_IsMainThread(), "Wrong thread!");
  MOZ_ASSERT(aReplyRunnable);

  // We accept canceling seek request only if it's currently seeking.
  if (mState != Seeking) {
    aReplyRunnable->SetReply(
        ErrorResponse(u"FM radio currently not seeking"_ns));
    NS_DispatchToMainThread(aReplyRunnable);
    return;
  }

  // Cancel the seek immediately to prevent it from completing.
  hal::CancelFMRadioSeek();

  TransitionState(ErrorResponse(u"Seek action is cancelled"_ns), Enabled);

  aReplyRunnable->SetReply(SuccessResponse());
  NS_DispatchToMainThread(aReplyRunnable);
}

void FMRadioService::SetRDSGroupMask(uint32_t aRDSGroupMask) {
  mRDSGroupMask = aRDSGroupMask;
  if (hal::IsFMRadioOn() && mRDSEnabled) {
    DebugOnly<bool> enabled =
        hal::EnableRDS(mRDSGroupMask | DOM_PARSED_RDS_GROUPS);
    MOZ_ASSERT(enabled);
  }
}

void FMRadioService::EnableRDS(FMRadioReplyRunnable* aReplyRunnable) {
  MOZ_ASSERT(NS_IsMainThread(), "Wrong thread!");
  MOZ_ASSERT(aReplyRunnable);

  if (hal::IsFMRadioOn()) {
    if (!hal::EnableRDS(mRDSGroupMask | DOM_PARSED_RDS_GROUPS)) {
      aReplyRunnable->SetReply(ErrorResponse(u"Could not enable RDS"_ns));
      NS_DispatchToMainThread(aReplyRunnable);
      return;
    }
  }

  mRDSEnabled = true;

  aReplyRunnable->SetReply(SuccessResponse());
  NS_DispatchToMainThread(aReplyRunnable);

  DispatchFMRadioEventToMainThread(RDSEnabledChanged);
}

void FMRadioService::DisableRDS(FMRadioReplyRunnable* aReplyRunnable) {
  MOZ_ASSERT(NS_IsMainThread(), "Wrong thread!");
  MOZ_ASSERT(aReplyRunnable);

  if (hal::IsFMRadioOn()) {
    hal::DisableRDS();
  }

  aReplyRunnable->SetReply(SuccessResponse());
  NS_DispatchToMainThread(aReplyRunnable);

  if (mRDSEnabled) {
    mRDSEnabled = false;

    DispatchFMRadioEventToMainThread(RDSEnabledChanged);
  }
}

void FMRadioService::NotifyFMRadioEvent(FMRadioEventType aType) {
  mObserverList.Broadcast(aType);
}

void FMRadioService::Notify(const hal::FMRadioOperationInformation& aInfo) {
  switch (aInfo.operation()) {
    case hal::FM_RADIO_OPERATION_ENABLE:
      MOZ_ASSERT(hal::IsFMRadioOn());
      MOZ_ASSERT(mState == Disabling || mState == Enabling);

      // If we're disabling, disable the radio right now.
      if (mState == Disabling) {
        DoDisable();
        return;
      }

      // Fire success callback on the enable request.
      TransitionState(SuccessResponse(), Enabled);

      // To make sure the FM app will get the right frequency after the FM
      // radio is enabled, we have to set the frequency first.
      hal::SetFMRadioFrequency(mPendingFrequencyInKHz);

      // Update the current frequency without sending the`FrequencyChanged`
      // event, to make sure the FM app will get the right frequency when the
      // `EnabledChange` event is sent.
      mPendingFrequencyInKHz = hal::GetFMRadioFrequency();
      UpdatePowerState();

      // The frequency was changed from '0' to some meaningful number, so we
      // should send the `FrequencyChanged` event manually.
      NotifyFMRadioEvent(FrequencyChanged);

      if (mRDSEnabled) {
        mRDSEnabled = hal::EnableRDS(mRDSGroupMask | DOM_PARSED_RDS_GROUPS);
        if (!mRDSEnabled) {
          NotifyFMRadioEvent(RDSEnabledChanged);
        }
      }
      break;
    case hal::FM_RADIO_OPERATION_DISABLE:
      MOZ_ASSERT(mState == Disabling);

      mPISet = false;
      mPTYSet = false;
      memset(mPSName, 0, sizeof(mPSName));
      memset(mRadiotext, 0, sizeof(mRadiotext));
      TransitionState(SuccessResponse(), Disabled);
      UpdatePowerState();
      break;
    case hal::FM_RADIO_OPERATION_SEEK:

      // Seek action might be cancelled by SetFrequency(), we need to check if
      // the current state is Seeking.
      if (mState == Seeking) {
        TransitionState(SuccessResponse(), Enabled);
      }

      UpdateFrequency();
      break;
    case hal::FM_RADIO_OPERATION_TUNE:
      UpdateFrequency();
      break;
    default:
      MOZ_CRASH();
  }
}

/* This is defined by the RDS standard */
static const uint16_t sRDSToUnicodeMap[256] = {
    // The lower half differs from ASCII in 0x1F, 0x24, 0x5E, 0x7E
    // Most control characters are replaced with 0x20 (space)
    // 0x0-
    0x0020,
    0x0020,
    0x0020,
    0x0020,
    0x0020,
    0x0020,
    0x0020,
    0x0020,
    0x0020,
    0x0009,
    0x000A,
    0x000B,
    0x0020,
    0x00D0,
    0x0020,
    0x0020,

    // 0x1-
    0x0020,
    0x0020,
    0x0020,
    0x0020,
    0x0020,
    0x0020,
    0x0020,
    0x0020,
    0x0020,
    0x0020,
    0x0020,
    0x001B,
    0x0020,
    0x0020,
    0x0020,
    0x00AD,

    // 0x2-
    0x0020,
    0x0021,
    0x0022,
    0x0023,
    0x00A4,
    0x0025,
    0x0026,
    0x0027,
    0x0028,
    0x0029,
    0x002A,
    0x002B,
    0x002C,
    0x002D,
    0x002E,
    0x002F,

    // 0x3-
    0x0030,
    0x0031,
    0x0032,
    0x0033,
    0x0034,
    0x0035,
    0x0036,
    0x0037,
    0x0038,
    0x0039,
    0x003A,
    0x003B,
    0x003C,
    0x003D,
    0x003E,
    0x003F,

    // 0x4-
    0x0040,
    0x0041,
    0x0042,
    0x0043,
    0x0044,
    0x0045,
    0x0046,
    0x0047,
    0x0048,
    0x0049,
    0x004A,
    0x004B,
    0x004C,
    0x004D,
    0x004E,
    0x004F,

    // 0x5-
    0x0050,
    0x0051,
    0x0052,
    0x0053,
    0x0054,
    0x0055,
    0x0056,
    0x0057,
    0x0058,
    0x0059,
    0x005A,
    0x005B,
    0x005C,
    0x005D,
    0x2015,
    0x005F,

    // 0x6-
    0x2551,
    0x0061,
    0x0062,
    0x0063,
    0x0064,
    0x0065,
    0x0066,
    0x0067,
    0x0068,
    0x0069,
    0x006A,
    0x006B,
    0x006C,
    0x006D,
    0x006E,
    0x006F,

    // 0x7-
    0x0070,
    0x0071,
    0x0072,
    0x0073,
    0x0074,
    0x0075,
    0x0076,
    0x0077,
    0x0078,
    0x0079,
    0x007A,
    0x007B,
    0x007C,
    0x007D,
    0x00AF,
    0x007F,

    // 0x8-
    0x00E1,
    0x00E0,
    0x00E9,
    0x00E8,
    0x00ED,
    0x00EC,
    0x00F3,
    0x00F2,
    0x00FA,
    0x00F9,
    0x00D1,
    0x00C7,
    0x015E,
    0x00DF,
    0x00A1,
    0x0132,

    // 0x9-
    0x00E2,
    0x00E4,
    0x00EA,
    0x00EB,
    0x00EE,
    0x00EF,
    0x00F4,
    0x00F6,
    0x00FB,
    0x00FC,
    0x00F1,
    0x00E7,
    0x015F,
    0x011F,
    0x0131,
    0x0133,

    // 0xA-
    0x00AA,
    0x03B1,
    0x00A9,
    0x2030,
    0x011E,
    0x011B,
    0x0148,
    0x0151,
    0x03C0,
    0x20AC,
    0x00A3,
    0x0024,
    0x2190,
    0x2191,
    0x2192,
    0x2193,

    // 0xB-
    0x00BA,
    0x00B9,
    0x00B2,
    0x00B3,
    0x00B1,
    0x0130,
    0x0144,
    0x0171,
    0x03BC,
    0x00BF,
    0x00F7,
    0x00B0,
    0x00BC,
    0x00BD,
    0x00BE,
    0x00A7,

    // 0xC-
    0x00C1,
    0x00C0,
    0x00C9,
    0x00C8,
    0x00CD,
    0x00CC,
    0x00D3,
    0x00D2,
    0x00DA,
    0x00D9,
    0x0158,
    0x010C,
    0x0160,
    0x017D,
    0x00D0,
    0x013F,

    // 0xD-
    0x00C2,
    0x00C4,
    0x00CA,
    0x00CB,
    0x00CE,
    0x00CF,
    0x00D4,
    0x00D6,
    0x00DB,
    0x00DC,
    0x0159,
    0x010D,
    0x0161,
    0x017E,
    0x0111,
    0x0140,

    // 0xE-
    0x00C3,
    0x00C5,
    0x00C6,
    0x0152,
    0x0177,
    0x00DD,
    0x00D5,
    0x00D8,
    0x00DE,
    0x014A,
    0x0154,
    0x0106,
    0x015A,
    0x0179,
    0x0166,
    0x00F0,

    // 0xF-
    0x00E3,
    0x00E5,
    0x00E6,
    0x0153,
    0x0175,
    0x00FD,
    0x00F5,
    0x00F8,
    0x00FE,
    0x014B,
    0x0155,
    0x0107,
    0x015B,
    0x017A,
    0x0167,
    0x0020,
};

void FMRadioService::Notify(const hal::FMRadioRDSGroup& aRDSGroup) {
  uint16_t blocks[4];
  blocks[0] = aRDSGroup.blockA();
  blocks[1] = aRDSGroup.blockB();
  blocks[2] = aRDSGroup.blockC();
  blocks[3] = aRDSGroup.blockD();

  /* Bit 11 in block B determines whether this is a type B group. */
  uint16_t lastPI = blocks[1] & (1 << 11) ? blocks[2] : mLastPI;

  /* Update PI if it's not set or if we get two PI with the new value. */
  if ((mPI != blocks[0] && lastPI == blocks[0]) || !mPISet) {
    mPI = blocks[0];
    if (!mPISet) {
      mPSNameState = 0;
      mRadiotextState = 0;
      memset(mTempPSName, 0, sizeof(mTempPSName));
      memset(mTempRadiotext, 0, sizeof(mTempRadiotext));
    }
    mPISet = true;

    DispatchFMRadioEventToMainThread(PIChanged);
  }
  mLastPI = blocks[0];

  /* PTY is also updated using the same logic as PI */
  uint16_t pty = (blocks[1] >> 5) & 0x1F;
  if ((mPTY != pty && pty == mLastPTY) || !mPTYSet) {
    mPTY = pty;
    mPTYSet = true;

    DispatchFMRadioEventToMainThread(PTYChanged);
  }
  mLastPTY = pty;

  uint16_t grouptype = blocks[1] >> 11;
  switch (grouptype) {
    case 0:  // 0a
    case 1:  // 0b
    {
      uint16_t segmentAddr = (blocks[1] & 0x3);
      // mPSNameState is a bitmask that lets us ensure all segments
      // are received before updating the PS name.
      if (!segmentAddr) {
        mPSNameState = 1;
      } else {
        mPSNameState |= 1 << segmentAddr;
      }

      uint16_t offset = segmentAddr << 1;
      mTempPSName[offset] = sRDSToUnicodeMap[blocks[3] >> 8];
      mTempPSName[offset + 1] = sRDSToUnicodeMap[blocks[3] & 0xFF];

      if (mPSNameState != 0xF) {
        break;
      }

      mPSNameState = 0;
      if (memcmp(mTempPSName, mPSName, sizeof(mTempPSName))) {
        MutexAutoLock lock(mRDSLock);
        mPSNameSet = true;
        memcpy(mPSName, mTempPSName, sizeof(mTempPSName));

        DispatchFMRadioEventToMainThread(PSChanged);
      }
      break;
    }
    case 4:  // 2a Radiotext
    {
      uint16_t segmentAddr = (blocks[1] & 0xF);
      bool textAB = blocks[1] & (1 << 5);
      if (textAB != mRadiotextAB) {
        mRadiotextState = 0;
        memset(mTempRadiotext, 0, sizeof(mTempRadiotext));
        mRadiotextAB = textAB;
        MutexAutoLock lock(mRDSLock);
        memset(mRadiotext, 0, sizeof(mRadiotext));

        DispatchFMRadioEventToMainThread(RadiotextChanged);
      }

      // mRadiotextState is a bitmask that lets us ensure all segments
      // are received before updating the radiotext.
      if (!segmentAddr) {
        mRadiotextState = 1;
      } else {
        mRadiotextState |= 1 << segmentAddr;
      }

      uint8_t segment[4];
      segment[0] = blocks[2] >> 8;
      segment[1] = blocks[2] & 0xFF;
      segment[2] = blocks[3] >> 8;
      segment[3] = blocks[3] & 0xFF;

      uint16_t offset = segmentAddr << 2;
      bool done = false;
      for (int i = 0; i < 4; i++) {
        if (segment[i] == '\r') {
          mTempRadiotext[offset++] = 0;
          done = true;
        } else {
          mTempRadiotext[offset++] = sRDSToUnicodeMap[segment[i]];
        }
      }
      if (offset == 64) {
        done = true;
      }

      if (!done || (mRadiotextState + 1) != (1 << ((blocks[1] & 0xF) + 1)) ||
          !memcmp(mTempRadiotext, mRadiotext, sizeof(mTempRadiotext))) {
        break;
      }

      MutexAutoLock lock(mRDSLock);
      mRadiotextSet = true;
      memcpy(mRadiotext, mTempRadiotext, sizeof(mTempRadiotext));

      DispatchFMRadioEventToMainThread(RadiotextChanged);
      break;
    }
    case 5:  // 2b Radiotext
    {
      uint16_t segmentAddr = (blocks[1] & 0xF);
      bool textAB = blocks[1] & (1 << 5);
      if (textAB != mRadiotextAB) {
        mRadiotextState = 0;
        memset(mTempRadiotext, 0, sizeof(mTempRadiotext));
        mRadiotextAB = textAB;
        MutexAutoLock lock(mRDSLock);
        memset(mRadiotext, 0, sizeof(mRadiotext));

        DispatchFMRadioEventToMainThread(RadiotextChanged);
      }

      if (!segmentAddr) {
        mRadiotextState = 1;
      } else {
        mRadiotextState |= 1 << segmentAddr;
      }
      uint8_t segment[2];
      segment[0] = blocks[3] >> 8;
      segment[1] = blocks[3] & 0xFF;

      uint16_t offset = segmentAddr << 1;
      bool done = false;
      for (int i = 0; i < 2; i++) {
        if (segment[i] == '\r') {
          mTempRadiotext[offset++] = 0;
          done = true;
        } else {
          mTempRadiotext[offset++] = sRDSToUnicodeMap[segment[i]];
        }
      }
      if (offset == 32) {
        done = true;
      }

      if (!done || (mRadiotextState + 1) != (1 << ((blocks[1] & 0xF) + 1)) ||
          !memcmp(mTempRadiotext, mRadiotext, sizeof(mTempRadiotext))) {
        break;
      }

      MutexAutoLock lock(mRDSLock);
      mRadiotextSet = true;
      memcpy(mRadiotext, mTempRadiotext, sizeof(mTempRadiotext));

      DispatchFMRadioEventToMainThread(RadiotextChanged);
      break;
    }
    case 31:  // 15b Fast Tuning and Switching
    {
      uint16_t secondPty = (blocks[3] >> 5) & 0x1F;
      if (pty == mPTY || pty != secondPty) {
        break;
      }
      mPTY = pty;

      DispatchFMRadioEventToMainThread(PTYChanged);
      break;
    }
  }

  // Only notify users of raw RDS groups that they're interested in.
  // We always receive DOM_PARSED_RDS_GROUPS when RDS is enabled.
  if (!(mRDSGroupMask & (1 << grouptype))) {
    return;
  }

  uint64_t newgroup = blocks[0];
  newgroup <<= 16;
  newgroup |= blocks[1];
  newgroup <<= 16;
  newgroup |= blocks[2];
  newgroup <<= 16;
  newgroup |= blocks[3];

  MutexAutoLock lock(mRDSLock);
  mRDSGroup = newgroup;
  mRDSGroupSet = true;

  DispatchFMRadioEventToMainThread(NewRDSGroup);
}

void FMRadioService::UpdatePowerState() {
  bool enabled = hal::IsFMRadioOn();
  if (enabled != mEnabled) {
    mEnabled = enabled;
    NotifyFMRadioEvent(EnabledChanged);
  }

  if (mRequireWakeLock) {
    if (enabled) {
      // Acquire wake lock to keep playing FM when screen off.
      WakeLockCreate();
    } else {
      // Release wake lock when FM is paused/stopped.
      WakeLockRelease();
    }
  }
}

void FMRadioService::UpdateFrequency() {
  int32_t frequency = hal::GetFMRadioFrequency();
  if (mPendingFrequencyInKHz != frequency) {
    mPendingFrequencyInKHz = frequency;
    NotifyFMRadioEvent(FrequencyChanged);
    mPISet = false;
    mPTYSet = false;
    memset(mPSName, 0, sizeof(mPSName));
    memset(mRadiotext, 0, sizeof(mRadiotext));
    mRDSGroupSet = false;
    mPSNameSet = false;
    mRadiotextSet = false;
  }
}

void FMRadioService::WakeLockCreate() {
  if (!mWakeLock) {
    RefPtr<power::PowerManagerService> pmService =
        power::PowerManagerService::GetInstance();
    NS_ENSURE_TRUE_VOID(pmService);

    ErrorResult rv;
    mWakeLock = pmService->NewWakeLock(u"cpu"_ns, nullptr, rv);
  }
}

void FMRadioService::WakeLockRelease() {
  if (mWakeLock) {
    ErrorResult rv;
    mWakeLock->Unlock(rv);
    NS_WARN_IF(rv.Failed());
    mWakeLock = nullptr;
  }
}

// static
FMRadioService* FMRadioService::Singleton() {
  MOZ_ASSERT(XRE_IsParentProcess());
  MOZ_ASSERT(NS_IsMainThread());

  if (!sFMRadioService) {
    sFMRadioService = new FMRadioService();
    ClearOnShutdown(&sFMRadioService);
  }

  return sFMRadioService;
}

NS_IMPL_ISUPPORTS(FMRadioService, nsISettingsGetResponse, nsISettingsObserver,
                  nsISidlDefaultResponse)

END_FMRADIO_NAMESPACE
