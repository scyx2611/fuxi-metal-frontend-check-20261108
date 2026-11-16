#include <assert.h>
#include <math.h>
#include <stdio.h>
#include "FuxiTorchLevels.h"

int main(void)
{
    const float canonical[] = {0.0f, 0.25f, 0.5f, 0.85f, 1.0f};
    unsigned decoded = 99;
    for (unsigned index = 0; index <= 4; index++) {
        assert(fuxi_torch_ui_decode(canonical[index], &decoded));
        assert(decoded == index);
        assert(fuxi_torch_ui_encode(index) == canonical[index]);
        assert(fuxi_torch_ui_decode(fuxi_torch_ui_encode(index), &decoded));
        assert(decoded == index);
    }
    for (unsigned index = 4; index > 0; index--) {
        assert(fuxi_torch_ui_decode(canonical[index], &decoded));
        assert(decoded == index);
        assert(fuxi_torch_ui_decode(canonical[index - 1], &decoded));
        assert(decoded == index - 1);
    }
    for (unsigned index = 1; index < 4; index++) {
        float boundary = canonical[index];
        assert(fuxi_torch_ui_decode(nextafterf(boundary, 0.0f), &decoded));
        assert(decoded == index);
        assert(fuxi_torch_ui_decode(nextafterf(boundary, 1.0f), &decoded));
        assert(decoded == index + 1);
    }
    assert(fuxi_torch_ui_decode(nextafterf(0.0f, 1.0f), &decoded) && decoded == 1);
    assert(fuxi_torch_ui_decode(-0.0f, &decoded) && decoded == 0);
    unsigned previous = 0;
    for (unsigned sample = 0; sample <= 10000; sample++) {
        float value = (float)sample / 10000.0f;
        assert(fuxi_torch_ui_decode(value, &decoded));
        assert(decoded >= previous && decoded <= 4);
        assert((value == 0.0f) == (decoded == 0));
        previous = decoded;
    }
    const float invalid[] = {-1.0f, 1.01f, INFINITY, -INFINITY, NAN};
    for (unsigned index = 0; index < sizeof(invalid) / sizeof(invalid[0]); index++) {
        decoded = 99;
        assert(!fuxi_torch_ui_decode(invalid[index], &decoded));
        assert(decoded == 99);
    }
    assert(!fuxi_torch_ui_decode(0.5f, 0));
    assert(fuxi_torch_ui_encode(5) == 0.0f);
    puts("PASS flashlight canonical levels, both directions, float boundaries, 10001 monotonic samples and invalid requests");
    return 0;
}
