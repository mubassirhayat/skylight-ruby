require 'skylight/util/platform'

module Skylight
  # @api private
  # Whether or not the native extension is present
  @@has_native_ext = false

  def self.native?
    @@has_native_ext
  end

  def self.libskylight_path
    ENV['SKYLIGHT_LIB_PATH'] || File.expand_path("../native/#{Util::Platform.tuple}", __FILE__)
  end

  begin
    unless ENV["SKYLIGHT_DISABLE_AGENT"]
      lib = "#{libskylight_path}/libskylight.#{Util::Platform.libext}"

      if File.exist?(lib)
        # First attempt to require the native extension
        require 'skylight_native'

        # Attempt to link the dylib
        load_libskylight(lib)

        # If nothing was thrown, then the native extension is present
        @@has_native_ext = true
      end
    end
  rescue LoadError => e
    raise if ENV.key?("SKYLIGHT_REQUIRED")
  end

  unless Skylight.native?
    class Instrumenter
      def self.native_new(*args)
        allocate
      end
    end
  end

  # @api private
  def self.check_install_errors(config)
    # Note: An unsupported arch doesn't count as an error.
    install_log = File.expand_path("../../ext/install.log", __FILE__)

    if File.exist?(install_log) && File.read(install_log) =~ /ERROR/
      config.alert_logger.error \
          "[SKYLIGHT] [#{Skylight::VERSION}] The Skylight native extension failed to install. " \
          "Please check #{install_log} and notify support@skylight.io." \
          "The missing extension will not affect the functioning of your application."
    end
  end

  # @api private
  def self.warn_skylight_native_missing(config)
    # TODO: Dumping the error messages this way is pretty hacky
    is_rails = defined?(Rails)
    env_name = is_rails ? Rails.env : "development"

    if env_name == "development" || env_name == "test"
      config.alert_logger.warn \
          "[SKYLIGHT] [#{Skylight::VERSION}] Running Skylight in #{env_name} mode. " \
          "No data will be reported until you deploy your app.\n" \
          "(To disable this message, set `alert_log_file` in your config.)"
    else
      config.alert_logger.error \
          "[SKYLIGHT] [#{Skylight::VERSION}] The Skylight native extension for your platform wasn't found. " \
          "The monitoring portion of Skylight is only supported on production servers running 32- or " \
          "64-bit Linux. The missing extension will not affect the functioning of your application " \
          "and you can continue local development without data being reported. If you are on a " \
          "supported platform, please contact support at support@skylight.io."
    end
  end
end
