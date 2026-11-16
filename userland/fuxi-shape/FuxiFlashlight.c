#include <CoreFoundation/CoreFoundation.h>
#include <dispatch/dispatch.h>
#include <objc/message.h>
#include <objc/runtime.h>
#include <sys/sysctl.h>
#include <errno.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

static unsigned torch_hooked;
static unsigned torch_level;
static dispatch_source_t torch_timer;
static pthread_mutex_t torch_control_lock = PTHREAD_MUTEX_INITIALIZER;

static void torch_log(const char *message)
{
    (void)write(2, message, strlen(message));
}

static bool torch_ready(void)
{
    int level = 0;
    size_t size = sizeof(level);
    return sysctlbyname("kern.fuxitorch", &level, &size, NULL, 0) == 0 &&
        size == sizeof(level) && level >= 0 && level <= 4;
}

static bool torch_write(unsigned level)
{
    int value = (int)level;
    return sysctlbyname("kern.fuxitorch", NULL, NULL, &value, sizeof(value)) == 0;
}

static void torch_renew(void *context)
{
    (void)context;
    (void)pthread_mutex_lock(&torch_control_lock);
    unsigned level = __atomic_load_n(&torch_level, __ATOMIC_ACQUIRE);
    if (level && !torch_write(level)) {
        __atomic_store_n(&torch_level, 0u, __ATOMIC_RELEASE);
        torch_log("fuxi: SpringBoard: torch lease renewal failed; output will expire\n");
    }
    (void)pthread_mutex_unlock(&torch_control_lock);
}

static void torch_error(id *error, int code)
{
    if (error == NULL) return;
    CFErrorRef value = CFErrorCreate(kCFAllocatorDefault, kCFErrorDomainPOSIX, code, NULL);
    *error = ((id (*)(id, SEL))objc_msgSend)((id)value, sel_registerName("autorelease"));
}

static bool torch_set(id self, SEL selector, float level, id *error)
{
    (void)selector;
    if (!(level >= 0.0f && level <= 1.0f)) {
        torch_error(error, EINVAL);
        return false;
    }
    unsigned requested = (unsigned)(level * 4.0f);
    if ((float)requested / 4.0f < level) requested++;
    unsigned previous = __atomic_load_n(&torch_level, __ATOMIC_ACQUIRE);
    if (previous != requested)
        ((void (*)(id, SEL, id))objc_msgSend)(self, sel_registerName("willChangeValueForKey:"),
            (id)CFSTR("flashlightLevel"));
    (void)pthread_mutex_lock(&torch_control_lock);
    bool accepted = torch_write(requested);
    int failure = accepted ? 0 : errno;
    if (accepted) __atomic_store_n(&torch_level, requested, __ATOMIC_RELEASE);
    (void)pthread_mutex_unlock(&torch_control_lock);
    if (previous != requested) {
        ((void (*)(id, SEL, id))objc_msgSend)(self, sel_registerName("didChangeValueForKey:"),
            (id)CFSTR("flashlightLevel"));
    }
    if (!accepted) {
        torch_error(error, failure);
        return false;
    }
    if (previous != requested) {
        char message[144];
        int length = snprintf(message, sizeof(message),
            "fuxi: SpringBoard: native flashlight level=%u current=%umA\n", requested, requested * 20);
        if (length > 0 && length < (int)sizeof(message)) (void)write(2, message, (size_t)length);
    }
    return true;
}

static id torch_init(id self, SEL selector)
{
    struct objc_super parent = {self, class_getSuperclass(objc_getClass("AVFlashlight"))};
    return ((id (*)(struct objc_super *, SEL))objc_msgSendSuper)(&parent, selector);
}

static bool torch_available(id self, SEL selector)
{
    (void)self;
    (void)selector;
    return torch_ready();
}

static bool torch_overheated(id self, SEL selector)
{
    (void)self;
    (void)selector;
    return false;
}

static float torch_get(id self, SEL selector)
{
    (void)self;
    (void)selector;
    int level = 0;
    size_t size = sizeof(level);
    if (sysctlbyname("kern.fuxitorch", &level, &size, NULL, 0) ||
        size != sizeof(level) || level < 0 || level > 4) return 0.0f;
    return (float)level / 4.0f;
}

static bool torch_power_on(id self, SEL selector, id *error)
{
    (void)self;
    (void)selector;
    if (torch_ready()) return true;
    torch_error(error, EIO);
    return false;
}

static void torch_power_off(id self, SEL selector)
{
    (void)torch_set(self, selector, 0.0f, NULL);
}

static bool torch_signature(Method method, char result, unsigned count, char third)
{
    char encoding[16];
    if (method == NULL || method_getNumberOfArguments(method) != count) return false;
    method_getReturnType(method, encoding, sizeof(encoding));
    if (result == 'B') {
        if (encoding[0] != 'B' && encoding[0] != 'c') return false;
    } else if (encoding[0] != result) {
        return false;
    }
    if (third) {
        method_getArgumentType(method, 2, encoding, sizeof(encoding));
        if (encoding[0] != third) return false;
    }
    if (count == 4) {
        method_getArgumentType(method, 3, encoding, sizeof(encoding));
        if (encoding[0] != '^') return false;
    }
    return true;
}

bool fuxi_flashlight_hook(void);
bool fuxi_flashlight_hook(void)
{
    if (__atomic_load_n(&torch_hooked, __ATOMIC_ACQUIRE)) return true;
    if (!torch_ready()) return false;
    Class target = objc_getClass("AVFlashlight");
    if (target == NULL) return false;
    const struct {
        const char *name;
        IMP replacement;
        char result;
        unsigned count;
        char third;
    } hooks[] = {
        {"init", (IMP)torch_init, '@', 2, 0},
        {"isAvailable", (IMP)torch_available, 'B', 2, 0},
        {"isOverheated", (IMP)torch_overheated, 'B', 2, 0},
        {"flashlightLevel", (IMP)torch_get, 'f', 2, 0},
        {"setFlashlightLevel:withError:", (IMP)torch_set, 'B', 4, 'f'},
        {"turnPowerOnWithError:", (IMP)torch_power_on, 'B', 3, '^'},
        {"turnPowerOff", (IMP)torch_power_off, 'v', 2, 0},
    };
    Method methods[sizeof(hooks) / sizeof(hooks[0])];
    Method present = class_getClassMethod(target, sel_registerName("hasFlashlight"));
    if (!torch_signature(present, 'B', 2, 0)) return false;
    for (unsigned index = 0; index < sizeof(hooks) / sizeof(hooks[0]); index++) {
        methods[index] = class_getInstanceMethod(target, sel_registerName(hooks[index].name));
        if (!torch_signature(methods[index], hooks[index].result, hooks[index].count, hooks[index].third)) {
            torch_log("fuxi: SpringBoard: AVFlashlight ABI mismatch; torch hooks refused\n");
            return false;
        }
    }
    torch_timer = dispatch_source_create(DISPATCH_SOURCE_TYPE_TIMER, 0, 0,
        dispatch_get_global_queue(DISPATCH_QUEUE_PRIORITY_DEFAULT, 0));
    if (torch_timer == NULL) return false;
    if (__atomic_exchange_n(&torch_hooked, 1u, __ATOMIC_ACQ_REL)) return true;
    (void)method_setImplementation(present, (IMP)torch_available);
    for (unsigned index = 0; index < sizeof(hooks) / sizeof(hooks[0]); index++)
        (void)method_setImplementation(methods[index], hooks[index].replacement);
    dispatch_source_set_event_handler_f(torch_timer, torch_renew);
    dispatch_source_set_timer(torch_timer, dispatch_time(DISPATCH_TIME_NOW, 500 * NSEC_PER_MSEC),
        500 * NSEC_PER_MSEC, 50 * NSEC_PER_MSEC);
    dispatch_resume(torch_timer);
    torch_log("fuxi: SpringBoard: native AVFlashlight hooks ready; 500ms lease heartbeat\n");
    return true;
}
