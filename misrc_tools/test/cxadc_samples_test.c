// SPDX-License-Identifier: GPL-3.0-or-later
#include "../misrc_gui/input/cxadc_samples.h"
#include <assert.h>
int main(void) {
    for(int code=0; code<256; code++) {
        int sample=cxadc_decode_sample_8bit((uint8_t)code);
        assert(sample==(code-128)*16);
        assert(sample/16+128==code);
        if(code)assert(sample>cxadc_decode_sample_8bit((uint8_t)(code-1)));
    }
    assert(cxadc_decode_sample_8bit(0)==-2048);
    assert(cxadc_decode_sample_8bit(128)==0);
    assert(cxadc_decode_sample_8bit(255)==2032);
    return 0;
}
