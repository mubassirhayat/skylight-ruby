require 'rbconfig'
require 'mkmf'
require 'yaml'
require 'logger'
require 'fileutils'

$:.unshift File.expand_path("../../lib", __FILE__)
require 'skylight/version'
require 'skylight/util/multi_io'
require 'skylight/util/native_ext_fetcher'
require 'skylight/util/platform'

include Skylight::Util

SKYLIGHT_INSTAL_LOG = File.expand_path("../install.log", __FILE__)
SKYLIGHT_REQUIRED   = ENV.key?("SKYLIGHT_REQUIRED") && ENV['SKYLIGHT_REQUIRED'] !~ /^false$/i
SKYLIGHT_LIB_PATH   = ENV['SKYLIGHT_LIB_PATH'] || File.expand_path('.')
SKYLIGHT_FETCH_LIB  = !ENV.key?('SKYLIGHT_FETCH_LIB') || ENV['SKYLIGHT_REQUIRED'] !~ /^false$/i
LOG                 = Logger.new(MultiIO.new(STDOUT, File.open(SKYLIGHT_INSTAL_LOG, 'a')))

# Handles terminating in the case of a failure. If we have a bug, we do not
# want to break our customer's deploy, but extconf.rb requires a Makefile to be
# present upon a successful exit. To satisfy this requirement, we create a
# dummy Makefile.
def fail(msg, type=:error)
  LOG.send type, msg

  if SKYLIGHT_REQUIRED
    exit 1
  else
    File.open("Makefile", "w") do |file|
      file.puts "default:"
      file.puts "install:"
    end

    exit
  end
end

#
# === Setup paths
#
root              = File.expand_path('../', __FILE__)
libpath           = File.expand_path(SKYLIGHT_LIB_PATH)
libskylight       = File.expand_path("libskylight.#{Platform.libext}", libpath)
libskylight_yml   = File.expand_path('libskylight.yml', root)
skylight_dlopen_h = File.expand_path("skylight_dlopen.h", libpath)
skylight_dlopen_c = File.expand_path("skylight_dlopen.c", libpath)

LOG.info "SKYLIGHT_LIB_PATH=#{libpath}"

# If libskylight is not present, fetch it
unless File.exist?(libskylight)
  if !SKYLIGHT_FETCH_LIB
    fail "libskylight.#{LIBEXT} not found -- remote download disabled; aborting install"
  end

  # Ensure that libskylight.yml is present and load it
  unless File.exist?(libskylight_yml)
    fail "`#{libskylight_yml}` does not exist"
  end

  unless libskylight_info = YAML.load_file(libskylight_yml)
    fail "`#{libskylight_yml}` does not contain data"
  end

  unless version = libskylight_info["version"]
    fail "libskylight version missing from `#{libskylight_yml}`"
  end

  unless checksums = libskylight_info["checksums"]
    fail "libskylight checksums missing from `#{libskylight_yml}`"
  end

  unless checksum = checksums[Platform.tuple]
    fail "no checksum entry for requested architecture -- " \
             "this probably means the requested architecture is not supported; " \
             "platform=#{Platform.tuple}; available=#{checksums.keys}", :info
  end

  begin
    res = NativeExtFetcher.fetch(
      version: version,
      target: File.expand_path('.'),
      checksum: checksum,
      arch: Platform.tuple,
      required: SKYLIGHT_REQUIRED,
      platform: Platform.tuple,
      logger: LOG)

    unless res
      fail "could not fetch archive -- aborting skylight native extension build"
    end
  rescue => e
    fail "unable to fetch native extension; msg=#{e.message}\n#{e.backtrace.join("\n")}"
  end
end

#
#
# ===== By this point, libskylight is present =====
#
#

# Flag -std=c99 required for older build systems
$CFLAGS << " -std=c99 -pedantic" # -Wall
$VPATH  << SKYLIGHT_LIB_PATH

# Where the ruby binding src is
SRC_PATH = File.expand_path('..', __FILE__)

$srcs = Dir[File.expand_path("*.c", SRC_PATH)].map { |f| File.basename(f) }
$srcs << "skylight_dlopen.c" # From libskylight dist

def find_file(file, root)
  path = File.expand_path(file, root)

  unless File.exist?(path)
    fail "#{file} missing; path=#{root}"
  end
end

# Make sure that the files are present
find_file 'skylight_dlopen.h', SKYLIGHT_LIB_PATH
find_file 'skylight_dlopen.c', SKYLIGHT_LIB_PATH
find_header 'skylight_dlopen.h', SKYLIGHT_LIB_PATH
have_header 'dlfcn.h' or fail "could not create Makefile; dlfcn.h missing"

# For escaping the GVL
unless have_func('rb_thread_call_without_gvl', 'ruby/thread.h')
  have_func('rb_thread_call_without_gvl')
end

# TODO: Compute the relative path to the location
create_makefile 'skylight_native', File.expand_path('..', __FILE__) # or fail "could not create makefile"
