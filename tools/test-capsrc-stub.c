#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "FuxiCaptureStubLogic.h"

typedef struct {
	bool array_set;
	bool u64_set;
	uint64_t u64_val;
	int alloc_should_fail;
	int live_arrays;
} fake_reply_env;

static int32_t
simulate_dispatch(uint32_t service_bit, uint32_t opcode, fake_reply_env *env, bool has_reply)
{
	fx_capsrc_action action = fuxi_capsrc_classify_op(service_bit, opcode);
	if (action == FX_CAPSRC_ACT_EMPTY_ARRAY) {
		if (!has_reply) {
			return 0;
		}
		if (env->alloc_should_fail) {
			return FX_CAPSRC_ERR_ALLOC;
		}
		env->live_arrays++;
		env->array_set = true;
		env->live_arrays--;
		return 0;
	}
	if (action == FX_CAPSRC_ACT_ZERO_U64) {
		if (has_reply) {
			env->u64_set = true;
			env->u64_val = 0u;
		}
		return 0;
	}
	return FX_CAPSRC_ERR_UNSUPPORTED;
}

int
main(void)
{
	uint32_t mask = 99u;
	char fourcc[5];

	/* Default off and out-of-range ADT masks */
	assert(!fuxi_capsrc_decode_adt(0u, &mask) && mask == 0u);
	assert(!fuxi_capsrc_decode_adt(16u, &mask) && mask == 0u);
	assert(!fuxi_capsrc_decode_adt(0xffffffffu, &mask) && mask == 0u);
	assert(!fuxi_capsrc_decode_adt(1u, NULL));

	/* Valid bitmasks 1..15 */
	for (uint32_t raw = 1u; raw <= FX_CAPSRC_MASK_ALL; raw++) {
		assert(fuxi_capsrc_decode_adt(raw, &mask) && mask == raw);
	}
	assert(fuxi_capsrc_decode_adt(FX_CAPSRC_MASK_DEFAULT, &mask) && mask == 7u);

	/* Service table integrity */
	assert(FX_CAPSRC_SERVICE_COUNT == 4u);
	assert(strcmp(fuxi_capsrc_services[0].mach_service, "com.apple.coremedia.capturesource") == 0);
	assert(strcmp(fuxi_capsrc_services[1].mach_service, "com.apple.coremedia.capturesession") == 0);
	assert(strcmp(fuxi_capsrc_services[2].mach_service,
	    "com.apple.coremedia.capture.deferred.containermanager.xpc") == 0);
	assert(strcmp(fuxi_capsrc_services[3].mach_service, "com.apple.coremedia.flashlight") == 0);
	uint32_t combined_bits = 0u;
	for (size_t idx = 0; idx < FX_CAPSRC_SERVICE_COUNT; idx++) {
		assert((combined_bits & fuxi_capsrc_services[idx].bit) == 0u);
		combined_bits |= fuxi_capsrc_services[idx].bit;
	}
	assert(combined_bits == FX_CAPSRC_MASK_ALL);
	assert(FX_CAPSRC_MASK_CAMERA_OWNED ==
	    (FX_CAPSRC_BIT_SOURCE | FX_CAPSRC_BIT_SESSION | FX_CAPSRC_BIT_DEFERRED | FX_CAPSRC_BIT_FLASHLIGHT));

	/* Camera coexistence: yield camera-owned CoreMedia XPC services iff fuxi,camera!=0 && fuxi,camera-dylib!=0 */
	assert(!fuxi_capsrc_camera_owns_servers(0u, 0u));
	assert(!fuxi_capsrc_camera_owns_servers(7u, 0u));
	assert(!fuxi_capsrc_camera_owns_servers(8u, 0u));
	assert(!fuxi_capsrc_camera_owns_servers(0u, 1u));
	assert(fuxi_capsrc_camera_owns_servers(1u, 1u));
	assert(fuxi_capsrc_camera_owns_servers(7u, 1u));
	assert(fuxi_capsrc_camera_owns_servers(8u, 2u));

	uint32_t yielded = 99u;
	for (uint32_t raw = 1u; raw <= FX_CAPSRC_MASK_ALL; raw++) {
		/* fuxi,camera==0 or fuxi,camera-dylib==0: 100% unchanged legacy stub behavior */
		assert(fuxi_capsrc_resolve_mask(raw, 0u, 0u, &yielded) == raw && yielded == 0u);
		assert(fuxi_capsrc_resolve_mask(raw, 7u, 0u, &yielded) == raw && yielded == 0u);
		assert(fuxi_capsrc_resolve_mask(raw, 0u, 1u, &yielded) == raw && yielded == 0u);

		/* fuxi,camera!=0 && fuxi,camera-dylib!=0: yield all 4 camera-owned services */
		assert(fuxi_capsrc_resolve_mask(raw, 7u, 1u, &yielded) == 0u && yielded == raw);
		assert(fuxi_capsrc_resolve_mask(raw, 8u, 1u, &yielded) == 0u && yielded == raw);
	}
	/* Edge cases: NULL out pointer, zero request, out-of-range high bits */
	assert(fuxi_capsrc_resolve_mask(FX_CAPSRC_MASK_DEFAULT, 7u, 1u, NULL) == 0u);
	assert(fuxi_capsrc_resolve_mask(FX_CAPSRC_MASK_DEFAULT, 0u, 0u, NULL) == FX_CAPSRC_MASK_DEFAULT);
	assert(fuxi_capsrc_resolve_mask(0u, 7u, 1u, &yielded) == 0u && yielded == 0u);
	assert(fuxi_capsrc_resolve_mask(0u, 0u, 0u, &yielded) == 0u && yielded == 0u);
	assert(fuxi_capsrc_resolve_mask(0xffffffffu, 0u, 0u, &yielded) == FX_CAPSRC_MASK_ALL && yielded == 0u);
	assert(fuxi_capsrc_resolve_mask(0xffffffffu, 7u, 1u, &yielded) == 0u && yielded == FX_CAPSRC_MASK_ALL);

	/* Opcode classification */
	assert(fuxi_capsrc_classify_op(FX_CAPSRC_BIT_SOURCE, FX_CAPSRC_OP_CSRC) == FX_CAPSRC_ACT_EMPTY_ARRAY);
	assert(fuxi_capsrc_classify_op(FX_CAPSRC_BIT_SOURCE, FX_CAPSRC_OP_MJDS) == FX_CAPSRC_ACT_ZERO_U64);
	assert(fuxi_capsrc_classify_op(FX_CAPSRC_BIT_SOURCE, 0x73737569u) == FX_CAPSRC_ACT_UNSUPPORTED);
	assert(fuxi_capsrc_classify_op(FX_CAPSRC_BIT_SOURCE, 0u) == FX_CAPSRC_ACT_UNSUPPORTED);
	assert(fuxi_capsrc_classify_op(FX_CAPSRC_BIT_SESSION, FX_CAPSRC_OP_CSRC) == FX_CAPSRC_ACT_UNSUPPORTED);
	assert(fuxi_capsrc_classify_op(FX_CAPSRC_BIT_DEFERRED, FX_CAPSRC_OP_CSRC) == FX_CAPSRC_ACT_UNSUPPORTED);
	assert(fuxi_capsrc_classify_op(FX_CAPSRC_BIT_FLASHLIGHT, FX_CAPSRC_OP_CSRC) == FX_CAPSRC_ACT_UNSUPPORTED);

	/* FourCC formatting */
	fuxi_capsrc_format_fourcc(FX_CAPSRC_OP_CSRC, fourcc);
	assert(strcmp(fourcc, "csrc") == 0);
	fuxi_capsrc_format_fourcc(FX_CAPSRC_OP_MJDS, fourcc);
	assert(strcmp(fourcc, "mjds") == 0);
	fuxi_capsrc_format_fourcc(0x007f0141u, fourcc);
	assert(strcmp(fourcc, "???A") == 0);
	fuxi_capsrc_format_fourcc(FX_CAPSRC_OP_CSRC, NULL);

	/* Simulated dispatch & memory lifecycle */
	fake_reply_env env = {0};
	assert(simulate_dispatch(FX_CAPSRC_BIT_SOURCE, FX_CAPSRC_OP_CSRC, &env, true) == 0);
	assert(env.array_set && env.live_arrays == 0);

	memset(&env, 0, sizeof(env));
	env.alloc_should_fail = 1;
	assert(simulate_dispatch(FX_CAPSRC_BIT_SOURCE, FX_CAPSRC_OP_CSRC, &env, true) == FX_CAPSRC_ERR_ALLOC);
	assert(!env.array_set && env.live_arrays == 0);

	memset(&env, 0, sizeof(env));
	assert(simulate_dispatch(FX_CAPSRC_BIT_SOURCE, FX_CAPSRC_OP_CSRC, &env, false) == 0);
	assert(!env.array_set);

	memset(&env, 0, sizeof(env));
	env.u64_val = 42u;
	assert(simulate_dispatch(FX_CAPSRC_BIT_SOURCE, FX_CAPSRC_OP_MJDS, &env, true) == 0);
	assert(env.u64_set && env.u64_val == 0u);

	memset(&env, 0, sizeof(env));
	assert(simulate_dispatch(FX_CAPSRC_BIT_SESSION, 0x63736573u, &env, true) == FX_CAPSRC_ERR_UNSUPPORTED);
	assert(!env.array_set && !env.u64_set);

	puts("PASS: FuxiCaptureStubLogic ADT decode, camera yield, service table, opcode dispatch, and fourcc format");
	return 0;
}
