#include <dlfcn.h>
#include <ruby.h>
#include <skylight_dlopen.h>

#ifdef HAVE_RUBY_ENCODING_H
#include <ruby/encoding.h>
#endif

#define TO_S(VAL) \
  RSTRING_PTR(rb_funcall(VAL, rb_intern("to_s"), 0))

#define CHECK_TYPE(VAL, T)                        \
  do {                                            \
    if (TYPE(VAL) != T) {                         \
      rb_raise(rb_eArgError, "expected " #VAL " to be " #T " but was '%s' (%s [%i])", \
                TO_S(VAL), rb_obj_classname(VAL), TYPE(VAL)); \
      return Qnil;                                \
    }                                             \
  } while(0)

#define CHECK_NUMERIC(VAL)                        \
  do {                                            \
    if (TYPE(VAL) != T_BIGNUM &&                  \
        TYPE(VAL) != T_FIXNUM) {                  \
      rb_raise(rb_eArgError, "expected " #VAL " to be numeric but was '%s' (%s [%i])", \
                TO_S(VAL), rb_obj_classname(VAL), TYPE(VAL)); \
      return Qnil;                                \
    }                                             \
  } while(0)                                      \

#define BUF2STR(buf)                            \
  ({                                            \
    sky_buf_t b = (buf);                        \
    VALUE str = rb_str_new(b.data, b.len);      \
    rb_enc_associate(str, rb_utf8_encoding());  \
    str;                                        \
  })

#define STR2BUF(str)           \
  ({                           \
    sky_buf_t buf;             \
    VALUE s = (str);           \
    buf.data = RSTRING_PTR(s); \
    buf.len = RSTRING_LEN(s);  \
    buf;                       \
  })

#define CHECK_FFI(success, message)               \
  do {                                            \
    if ((success) < 0 ) {                         \
      rb_raise(rb_eRuntimeError, message);        \
      return Qnil;                                \
    }                                             \
  } while(0)

#define My_Struct(name, Type, msg)                \
  Get_Struct(name, self, Type, msg);              \

#define Transfer_My_Struct(name, Type, msg)       \
  My_Struct(name, Type, msg);                     \
  DATA_PTR(self) = NULL;                          \

#define Transfer_Struct(name, obj, Type, msg)     \
  Get_Struct(name, obj, Type, msg);               \
  DATA_PTR(obj) = NULL;                           \

#define Get_Struct(name, obj, Type, msg)          \
  Type name;                                      \
  Data_Get_Struct(obj, Type, name);               \
  if (name == NULL) {                             \
    rb_raise(rb_eRuntimeError, "%s", msg);        \
  }

/**
 * Ruby GVL helpers
 */

#if defined(HAVE_RB_THREAD_CALL_WITHOUT_GVL) && \
    defined(HAVE_RUBY_THREAD_H)

// Ruby 2.0+
#include <ruby/thread.h>
typedef void* (*blocking_fn_t)(void*);
#define WITHOUT_GVL(fn, a) \
  rb_thread_call_without_gvl((blocking_fn_t)(fn), (a), 0, 0)

// Ruby 1.9
#elif defined(HAVE_RB_THREAD_BLOCKING_REGION)

typedef VALUE (*blocking_fn_t)(void*);
#define WITHOUT_GVL(fn, a) \
  rb_thread_blocking_region((blocking_fn_t)(fn), (a), 0, 0)


#endif


/**
 * Ruby types defined here
 */

VALUE rb_mSkylight;
VALUE rb_mUtil;
VALUE rb_cClock;
VALUE rb_cTrace;
VALUE rb_cInstrumenter;

static const char* no_instrumenter_msg =
  "Instrumenter not currently running";

static const char* consumed_trace_msg =
  "Trace objects cannot be used once it has been submitted to the instrumenter";

static VALUE
load_libskylight(VALUE klass, VALUE path) {
  int res;

  CHECK_TYPE(path, T_STRING);

  // Already loaded
  if (sky_hrtime != 0) {
    return Qnil;
  }

  res = sky_load_libskylight(StringValueCStr(path));

  if (res < 0) {
    rb_raise(rb_eRuntimeError, "[SKYLIGHT] dlerror; msg=%s", dlerror());
    return Qnil;
  }

  return Qnil;
}

/*
 *
 * class Skylight::Util::Clock
 *
 */

static VALUE
clock_high_res_time(VALUE self) {
  return ULL2NUM(sky_hrtime());
}

/*
 *
 * class Skylight::Instrumenter
 *
 */

static VALUE
instrumenter_new(VALUE klass, VALUE rb_env) {
  sky_instrumenter_t* instrumenter;
  sky_buf_t env[256];

  CHECK_TYPE(rb_env, T_ARRAY);

  if (RARRAY_LEN(rb_env) >= 256) {
    rb_raise(rb_eArgError, "environment array too long");
    return Qnil;
  }

  int i;
  int envc = (int) RARRAY_LEN(rb_env);

  for (i = 0; i < envc; ++i) {
    VALUE val = rb_ary_entry(rb_env, i);

    // Make sure it is a string
    CHECK_TYPE(val, T_STRING);

    env[i] = STR2BUF(val);
  }

  CHECK_FFI(
      sky_instrumenter_new(env, envc, &instrumenter),
      "failed to initialize instrumenter");

  return Data_Wrap_Struct(rb_cInstrumenter, NULL, sky_instrumenter_free, instrumenter);
}

static void*
instrumenter_start_nogvl(sky_instrumenter_t* instrumenter) {
  /*
   * Cannot use CHECK_FFI in here
   */

  if (sky_instrumenter_start(instrumenter) == 0) {
    return (void*) Qtrue;
  }
  else {
    return (void*) Qfalse;
  }
}

static VALUE
instrumenter_start(VALUE self) {
  My_Struct(instrumenter, sky_instrumenter_t*, no_instrumenter_msg);

  return (VALUE) WITHOUT_GVL(instrumenter_start_nogvl, instrumenter);
}

static VALUE
instrumenter_stop(VALUE self) {
  My_Struct(instrumenter, sky_instrumenter_t*, no_instrumenter_msg);

  CHECK_FFI(
      sky_instrumenter_stop(instrumenter),
      "native Instrumenter#stop failed");

  return Qnil;
}

static VALUE
instrumenter_submit_trace(VALUE self, VALUE rb_trace) {
  My_Struct(instrumenter, sky_instrumenter_t*, no_instrumenter_msg);
  Transfer_Struct(trace, rb_trace, sky_trace_t*, consumed_trace_msg);

  CHECK_FFI(
      sky_instrumenter_submit_trace(instrumenter, trace),
      "native Instrumenter#submit_trace failed");

  return Qnil;
}

/*
 *
 * class Skylight::Trace
 *
 */

static VALUE
trace_new(VALUE klass, VALUE start, VALUE uuid, VALUE endpoint) {
  CHECK_NUMERIC(start);
  CHECK_TYPE(uuid, T_STRING);
  CHECK_TYPE(endpoint, T_STRING);

  sky_trace_t* trace;

  CHECK_FFI(
      sky_trace_new(NUM2ULL(start), STR2BUF(uuid), STR2BUF(endpoint), &trace),
      "native Trace#new failed");

  return Data_Wrap_Struct(rb_cTrace, NULL, sky_trace_free, trace);
}

static VALUE
trace_get_started_at(VALUE self) {
  uint64_t start;
  My_Struct(trace, sky_trace_t*, consumed_trace_msg);

  CHECK_FFI(
      sky_trace_start(trace, &start),
      "native Trace#started_at failed");

  return ULL2NUM(start);
}

static VALUE
trace_get_endpoint(VALUE self) {
  sky_buf_t endpoint;
  My_Struct(trace, sky_trace_t*, consumed_trace_msg);

  CHECK_FFI(
      sky_trace_endpoint(trace, &endpoint),
      "native Trace#endpoint failed");

  return BUF2STR(endpoint);
}

static VALUE
trace_set_endpoint(VALUE self, VALUE endpoint) {
  CHECK_TYPE(endpoint, T_STRING);
  My_Struct(trace, sky_trace_t*, consumed_trace_msg);

  CHECK_FFI(
      sky_trace_set_endpoint(trace, STR2BUF(endpoint)),
      "native Trace#set_endpoint failed");

  return Qnil;
}

static VALUE
trace_get_uuid(VALUE self) {
  sky_buf_t uuid;
  My_Struct(trace, sky_trace_t*, consumed_trace_msg);

  CHECK_FFI(
      sky_trace_uuid(trace, &uuid),
      "native Trace#uuid failed");

  return BUF2STR(uuid);
}

static VALUE
trace_start_span(VALUE self, VALUE time, VALUE category) {
  uint32_t span;
  My_Struct(trace, sky_trace_t*, consumed_trace_msg);

  CHECK_NUMERIC(time);
  CHECK_TYPE(category, T_STRING);

  CHECK_FFI(
      sky_trace_instrument(trace, NUM2ULL(time), STR2BUF(category), &span),
      "native Trace#start_span failed");

  return UINT2NUM(span);
}

static VALUE
trace_stop_span(VALUE self, VALUE span, VALUE time) {
  My_Struct(trace, sky_trace_t*, consumed_trace_msg);

  CHECK_NUMERIC(time);
  CHECK_TYPE(span, T_FIXNUM);

  CHECK_FFI(
      sky_trace_span_done(trace, FIX2UINT(span), NUM2ULL(time)),
      "native Trace#stop_span failed");

  return Qnil;
}

static VALUE
trace_span_set_title(VALUE self, VALUE span, VALUE title) {
  My_Struct(trace, sky_trace_t*, consumed_trace_msg);

  CHECK_TYPE(span, T_FIXNUM);
  CHECK_TYPE(title, T_STRING);

  CHECK_FFI(
      sky_trace_span_set_title(trace, FIX2UINT(span), STR2BUF(title)),
      "native Trace#span_set_title failed");

  return Qnil;
}

static VALUE
trace_span_set_description(VALUE self, VALUE span, VALUE desc) {
  My_Struct(trace, sky_trace_t*, consumed_trace_msg);

  CHECK_TYPE(span, T_FIXNUM);
  CHECK_TYPE(desc, T_STRING);

  CHECK_FFI(
      sky_trace_span_set_desc(trace, FIX2UINT(span), STR2BUF(desc)),
      "native Trace#span_set_description failed");

  return Qnil;
}

void Init_skylight_native() {
  rb_mSkylight = rb_define_module("Skylight");
  rb_define_singleton_method(rb_mSkylight, "load_libskylight", load_libskylight, 1);

  rb_mUtil  = rb_define_module_under(rb_mSkylight, "Util");
  rb_cClock = rb_define_class_under(rb_mUtil, "Clock", rb_cObject);
  rb_define_method(rb_cClock, "native_hrtime", clock_high_res_time, 0);

  rb_cTrace = rb_define_class_under(rb_mSkylight, "Trace", rb_cObject);
  rb_define_singleton_method(rb_cTrace, "native_new", trace_new, 3);
  rb_define_method(rb_cTrace, "native_get_started_at", trace_get_started_at, 0);
  rb_define_method(rb_cTrace, "native_get_endpoint", trace_get_endpoint, 0);
  rb_define_method(rb_cTrace, "native_set_endpoint", trace_set_endpoint, 1);
  rb_define_method(rb_cTrace, "native_get_uuid", trace_get_uuid, 0);
  rb_define_method(rb_cTrace, "native_start_span", trace_start_span, 2);
  rb_define_method(rb_cTrace, "native_stop_span", trace_stop_span, 2);
  rb_define_method(rb_cTrace, "native_span_set_title", trace_span_set_title, 2);
  rb_define_method(rb_cTrace, "native_span_set_description", trace_span_set_description, 2);

  rb_cInstrumenter = rb_define_class_under(rb_mSkylight, "Instrumenter", rb_cObject);
  rb_define_singleton_method(rb_cInstrumenter, "native_new", instrumenter_new, 1);
  rb_define_method(rb_cInstrumenter, "native_start", instrumenter_start, 0);
  rb_define_method(rb_cInstrumenter, "native_stop", instrumenter_stop, 0);
  rb_define_method(rb_cInstrumenter, "native_submit_trace", instrumenter_submit_trace, 1);
}
