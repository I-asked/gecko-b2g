// Copyright (c) the JPEG XL Project Authors. All rights reserved.
//
// Use of this source code is governed by a BSD-style
// license that can be found in the LICENSE file.

#ifndef LIB_JXL_JXL_CMS_H_
#define LIB_JXL_JXL_CMS_H_

// ICC profiles and color space conversions.

#include <jxl/cms_interface.h>

#ifdef __cplusplus
extern "C" {
#endif

const JxlCmsInterface* JxlGetDefaultCms();

#ifdef __cplusplus
}
#endif

#endif  // LIB_JXL_JXL_CMS_H_
