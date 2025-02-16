/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* vim: set ts=8 sts=2 et sw=2 tw=80: */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this file,
 * You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef mozilla_dom_TelephonyDialCallback_h
#define mozilla_dom_TelephonyDialCallback_h

#include "Telephony.h"
#include "mozilla/dom/MMICall.h"
#include "mozilla/dom/MobileConnectionBinding.h"
#include "mozilla/dom/Promise.h"
#include "mozilla/dom/ToJSValue.h"
#include "mozilla/dom/telephony/TelephonyCallback.h"
#include "nsCOMPtr.h"
#include "nsITelephonyService.h"
#include "nsJSUtils.h"
#include "nsString.h"

class nsPIDOMWindowInner;

namespace mozilla {
namespace dom {
namespace telephony {

class TelephonyDialCallback final : public TelephonyCallback,
                                    public nsITelephonyDialCallback {
 public:
  NS_DECL_ISUPPORTS_INHERITED
  NS_DECL_NSITELEPHONYDIALCALLBACK

  TelephonyDialCallback(nsPIDOMWindowInner* aWindow, Telephony* aTelephony,
                        Promise* aPromise);

  NS_FORWARD_NSITELEPHONYCALLBACK(TelephonyCallback::)

 private:
  ~TelephonyDialCallback() {}

  nsresult NotifyDialMMISuccess(JSContext* aCx, const MMIResult& aResult);

  nsCOMPtr<nsPIDOMWindowInner> mWindow;
  RefPtr<Telephony> mTelephony;

  nsString mServiceCode;
  RefPtr<MMICall> mMMICall;
};

}  // namespace telephony
}  // namespace dom
}  // namespace mozilla

#endif  // mozilla_dom_TelephonyDialCallback_h
