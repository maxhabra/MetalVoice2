#pragma once
// MetalVoice2 standalone integration: the vendored DSP only uses debug checks.
// Standard assertions replace WebRTC's Abseil-dependent debug logging machinery.
// No production RTC_CHECK or streamed logging expression is supported here.
#include <cassert>
#define RTC_DCHECK(x) assert(x)
#define RTC_DCHECK_EQ(a, b) assert((a) == (b))
#define RTC_DCHECK_NE(a, b) assert((a) != (b))
#define RTC_DCHECK_LT(a, b) assert((a) < (b))
#define RTC_DCHECK_LE(a, b) assert((a) <= (b))
#define RTC_DCHECK_GT(a, b) assert((a) > (b))
#define RTC_DCHECK_GE(a, b) assert((a) >= (b))
#define RTC_DCHECK_NOTREACHED() assert(false)
