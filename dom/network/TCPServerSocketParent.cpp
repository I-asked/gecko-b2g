/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* vim: set ts=8 sts=2 et sw=2 tw=80: */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this file,
 * You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "TCPServerSocket.h"
#include "TCPServerSocketParent.h"
#include "nsJSUtils.h"
#include "TCPSocket.h"
#include "TCPSocketParent.h"
#include "mozilla/Unused.h"
#include "mozilla/dom/BrowserParent.h"
#include "mozilla/dom/TCPServerSocketEvent.h"
#include "mozilla/dom/ContentParent.h"
#include "mozilla/dom/CanonicalBrowsingContext.h"
#include "mozilla/dom/WindowGlobalParent.h"

namespace mozilla::dom {

NS_IMPL_CYCLE_COLLECTION(TCPServerSocketParent, mServerSocket)
NS_IMPL_CYCLE_COLLECTING_ADDREF(TCPServerSocketParent)
NS_IMPL_CYCLE_COLLECTING_RELEASE(TCPServerSocketParent)

NS_INTERFACE_MAP_BEGIN_CYCLE_COLLECTION(TCPServerSocketParent)
  NS_INTERFACE_MAP_ENTRY(nsISupports)
NS_INTERFACE_MAP_END

void TCPServerSocketParent::ReleaseIPDLReference() {
  MOZ_ASSERT(mIPCOpen);
  mIPCOpen = false;
  this->Release();
}

void TCPServerSocketParent::AddIPDLReference() {
  MOZ_ASSERT(!mIPCOpen);
  mIPCOpen = true;
  this->AddRef();
}

TCPServerSocketParent::TCPServerSocketParent(PNeckoParent* neckoParent,
                                             uint16_t aLocalPort,
                                             uint16_t aBacklog,
                                             bool aUseArrayBuffers)
    : mNeckoParent(neckoParent), mIPCOpen(false) {
  mServerSocket =
      new TCPServerSocket(nullptr, aLocalPort, aUseArrayBuffers, aBacklog);
  mServerSocket->SetServerBridgeParent(this);
}

TCPServerSocketParent::~TCPServerSocketParent() = default;

void TCPServerSocketParent::Init() {
  NS_ENSURE_SUCCESS_VOID(mServerSocket->Init());
}

nsresult TCPServerSocketParent::SendCallbackAccept(TCPSocketParent* socket) {
  nsresult rv;

  nsString host;
  rv = socket->GetHost(host);
  if (NS_FAILED(rv)) {
    NS_ERROR("Failed to get host from nsITCPSocketParent");
    return NS_ERROR_FAILURE;
  }

  uint16_t port;
  rv = socket->GetPort(&port);
  if (NS_FAILED(rv)) {
    NS_ERROR("Failed to get port from nsITCPSocketParent");
    return NS_ERROR_FAILURE;
  }

  if (mNeckoParent) {
    if (mNeckoParent->SendPTCPSocketConstructor(socket, host, port)) {
      // Call |AddIPDLReference| after the consructor message is sent
      // successfully, otherwise |socket| could be leaked.
      socket->AddIPDLReference();

      mozilla::Unused << PTCPServerSocketParent::SendCallbackAccept(
          WrapNotNull(socket));
    } else {
      NS_ERROR("Sending data from PTCPSocketParent was failed.");
    }
  } else {
    NS_ERROR("The member value for NeckoParent is wrong.");
  }
  return NS_OK;
}

mozilla::ipc::IPCResult TCPServerSocketParent::RecvClose() {
  NS_ENSURE_TRUE(mServerSocket, IPC_OK());
  mServerSocket->Close();
  return IPC_OK();
}

void TCPServerSocketParent::ActorDestroy(ActorDestroyReason why) {
  if (mServerSocket) {
    mServerSocket->Close();
    mServerSocket = nullptr;
  }
  mNeckoParent = nullptr;
}

mozilla::ipc::IPCResult TCPServerSocketParent::RecvRequestDelete() {
  mozilla::Unused << Send__delete__(this);
  return IPC_OK();
}

void TCPServerSocketParent::OnConnect(TCPServerSocketEvent* event) {
  RefPtr<TCPSocket> socket = event->Socket();
  nsAutoCString origin;
  nsAutoCString url;
  bool isApp = false;
  nsAutoCString manifestURL;
  GetOrigin(origin, url, &isApp, manifestURL);
  socket->SetOrigin(origin, url, isApp, manifestURL);

  RefPtr<TCPSocketParent> socketParent = new TCPSocketParent();
  socketParent->SetSocket(socket);

  socket->SetSocketBridgeParent(socketParent);

  SendCallbackAccept(socketParent);
}

void TCPServerSocketParent::GetOrigin(nsAutoCString& aOrigin, nsAutoCString& aURL, bool* aIsApp,
                                      nsAutoCString& aManifestURL) {
  const PContentParent* content = Manager()->Manager();
  if (PBrowserParent* browser =
          SingleManagedOrNull(content->ManagedPBrowserParent())) {
    CanonicalBrowsingContext* browsingContext =
        static_cast<BrowserParent*>(browser)->GetBrowsingContext()->Top();
    if (!browsingContext) {
      return;
    }

    WindowGlobalParent* window = browsingContext->GetCurrentWindowGlobal();
    if (!window) {
      return;
    }

    nsCOMPtr<nsIPrincipal> principal = window->DocumentPrincipal();
    if (principal) {
      principal->GetOrigin(aOrigin);
      principal->GetSpec(aURL);
      nsCOMPtr<nsIPermissionManager> permMgr =
          mozilla::services::GetPermissionManager();
      uint32_t perm = nsIPermissionManager::DENY_ACTION;
      if (permMgr) {
        permMgr->TestExactPermissionFromPrincipal(
            principal, "networkstats-perm"_ns, &perm);
        *aIsApp = perm == nsIPermissionManager::ALLOW_ACTION;
      }
    }

    RefPtr<dom::Element> element = browsingContext->GetEmbedderElement();
    if (!element) {
      return;
    }

    RefPtr<dom::Element> parentElement = element->GetParentElement();
    if (parentElement) {
      nsAutoString manifestURLStr;
      nsAutoString tagName;
      parentElement->GetTagName(tagName);
      if (tagName.LowerCaseEqualsLiteral("web-view")) {
        parentElement->GetAttribute(u"data-manifest-url"_ns, manifestURLStr);
        aManifestURL.Assign(NS_ConvertUTF16toUTF8(manifestURLStr));
      }
    }
  }
}

}  // namespace mozilla::dom
