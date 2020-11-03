#include <algorithm>
#include <cinttypes>
#include <iostream>
#include <string>
#include <string_view>
#include <type_traits>
#include <variant>

#include "realm/db.hpp"
#include "realm/sync/history.hpp"
#include "realm/sync/instruction_applier.hpp"
#include "realm/sync/impl/clamped_hex_dump.hpp"
#include "realm/sync/noinst/client_history_impl.hpp"
#include "realm/sync/protocol.hpp"
#include "realm/sync/transform.hpp"
#include "realm/sync/changeset_parser.hpp"
#include "realm/sync/noinst/compression.hpp"
#include "realm/util/cli_args.hpp"
#include "realm/util/load_file.hpp"
#include "realm/util/safe_int_ops.hpp"
#include "realm/util/string_view.hpp"

using namespace realm::util;

template <typename T>
using ParseResult = std::optional<std::pair<T, StringView>>;

struct ServerIdentMessage {
    realm::sync::session_ident_type session_ident;
    realm::sync::SaltedFileIdent file_ident;

    static ParseResult<ServerIdentMessage> parse(StringView sv);
};

struct DownloadMessage {
    realm::sync::session_ident_type session_ident;
    realm::sync::SyncProgress progress;
    realm::sync::SaltedVersion latest_server_version;
    uint64_t downloadable_bytes;

    Buffer<char> uncompressed_body_buffer;
    std::vector<realm::sync::Transformer::RemoteChangeset> changesets;

    static ParseResult<DownloadMessage> parse(StringView sv, Logger* logger);
};

struct UploadMessage {
    realm::sync::session_ident_type session_ident;
    realm::sync::UploadCursor upload_progress;
    realm::sync::version_type locked_server_version;

    Buffer<char> uncompressed_body_buffer;
    std::vector<realm::sync::Changeset> changesets;

    static ParseResult<UploadMessage> parse(StringView sv, Logger* logger);
};

using Message = std::variant<ServerIdentMessage, DownloadMessage, UploadMessage>;

// These functions will parse the space/new-line delimited headers found at the beginning of
// messages and changesets.
std::pair<StringView, bool> parse_header_element(StringView sv, char)
{
    return {sv, true};
}

template <typename T, typename = std::enable_if_t<std::is_integral_v<std::remove_reference_t<T>>>>
std::pair<StringView, bool> parse_header_value(StringView sv, T&& cur_arg)
{
    char* end_ptr = nullptr;
    auto big_val = std::strtoimax(sv.data(), &end_ptr, 10);
    if (!int_cast_with_overflow_detect(big_val, cur_arg)) {
        return {StringView{}, false};
    }
    cur_arg = reinterpret_cast<T>(big_val);

    if (end_ptr == nullptr || errno == ERANGE) {
        return {StringView{}, false};
    }

    // TODO subtracting the end_ptr from a pointer to back() is pretty janky, maybe there's
    // a better way of doing this.
    const auto remaining_length = static_cast<std::size_t>(&sv.back() - end_ptr);
    return {StringView{end_ptr, remaining_length}, true};
}

std::pair<StringView, bool> parse_header_value(StringView sv, StringView& cur_arg)
{
    auto delim_at = std::find_if(sv.begin(), sv.end(), [](const auto& val) -> bool {
        return val == ' ';
    });

    if (delim_at == sv.end()) {
        return {StringView{}, false};
    }

    auto sub_str_len = std::distance(sv.begin(), delim_at);
    cur_arg = StringView(sv.begin(), sub_str_len);

    return {sv.substr(sub_str_len), true};
}

template <typename T, typename... Args>
std::pair<StringView, bool> parse_header_element(StringView sv, char end_delim, T&& cur_arg, Args&&... next_args)
{
    if (sv.empty()) {
        return {StringView{}, false};
    }
    bool ok_value_parse;
    std::tie(sv, ok_value_parse) = parse_header_value(sv, std::forward<T&&>(cur_arg));
    if (!ok_value_parse) {
        return {StringView{}, false};
    }

    if (sv.front() == ' ') {
        return parse_header_element(sv.substr(1), end_delim, next_args...);
    }
    if (sv.front() == end_delim) {
        return {sv.substr(1), true};
    }
    return {StringView{}, false};
}

template <typename... Args>
std::pair<StringView, bool> parse_header_line(StringView sv, char end_delim, Args&&... args)
{
    return parse_header_element(sv, end_delim, args...);
}

struct MessageBody {
    StringView body_view;
    StringView remaining;
    Buffer<char> uncompressed_body_buffer;
    static std::optional<MessageBody> parse(StringView sv, std::size_t compressed_body_size,
                                            std::size_t uncompressed_body_size, bool is_body_compressed,
                                            Logger* logger);
};

std::optional<MessageBody> MessageBody::parse(StringView sv, std::size_t compressed_body_size,
                                              std::size_t uncompressed_body_size, bool is_body_compressed,
                                              Logger* logger)
{
    MessageBody ret;
    if (is_body_compressed) {
        if (sv.size() < compressed_body_size) {
            logger->error("compressed message body is bigger (%1) than available bytes (%2)", compressed_body_size,
                          sv.size());
            return std::nullopt;
        }

        ret.uncompressed_body_buffer.set_size(uncompressed_body_size);
        auto ec = realm::_impl::compression::decompress(sv.data(), compressed_body_size,
                                                        ret.uncompressed_body_buffer.data(), uncompressed_body_size);
        if (ec) {
            logger->error("error decompressing message body: %1", ec.message());
            return std::nullopt;
        }

        ret.remaining = sv.substr(compressed_body_size);
        ret.body_view = StringView{ret.uncompressed_body_buffer.data(), uncompressed_body_size};
    }
    else {
        if (sv.size() < uncompressed_body_size) {
            logger->error("message body is bigger (%1) than available bytes (%2)", uncompressed_body_size, sv.size());
            return std::nullopt;
        }
        ret.body_view = sv.substr(0, uncompressed_body_size);
        ret.remaining = sv.substr(uncompressed_body_size);
    }

    return ret;
}

std::optional<std::pair<Message, StringView>> parse_message(StringView sv, Logger* logger)
{
    StringView message_type;
    bool parse_ok;
    std::tie(sv, parse_ok) = parse_header_element(sv, ' ', message_type);
    if (!parse_ok) {
        return std::nullopt;
    }

    if (message_type == "download") {
        return DownloadMessage::parse(sv, logger);
    }
    else if (message_type == "upload") {
        return UploadMessage::parse(sv, logger);
    }
    else if (message_type == "ident") {
        return ServerIdentMessage::parse(sv);
    }
    return std::nullopt;
}

std::optional<std::pair<ServerIdentMessage, StringView>> ServerIdentMessage::parse(StringView sv)
{
    ServerIdentMessage ret;
    bool parse_ok;

    std::tie(sv, parse_ok) =
        parse_header_line(sv, '\n', ret.session_ident, ret.file_ident.ident, ret.file_ident.salt);
    if (!parse_ok) {
        return std::nullopt;
    }

    return std::make_pair(std::move(ret), sv);
}

std::optional<std::pair<DownloadMessage, StringView>> DownloadMessage::parse(StringView sv, Logger* logger)
{
    DownloadMessage ret;
    int is_body_compressed;
    std::size_t uncompressed_body_size, compressed_body_size;
    bool parse_ok;

    std::tie(sv, parse_ok) =
        parse_header_line(sv, '\n', ret.session_ident, ret.progress.download.server_version,
                          ret.progress.download.last_integrated_client_version, ret.latest_server_version.version,
                          ret.latest_server_version.salt, ret.progress.upload.client_version,
                          ret.progress.upload.last_integrated_server_version, ret.downloadable_bytes,
                          is_body_compressed, uncompressed_body_size, compressed_body_size);
    if (!parse_ok) {
        logger->error("error parsing header line for download message");
        return std::nullopt;
    }

    auto maybe_message_body =
        MessageBody::parse(sv, compressed_body_size, uncompressed_body_size, is_body_compressed, logger);
    if (!maybe_message_body) {
        return std::nullopt;
    }
    ret.uncompressed_body_buffer = std::move(maybe_message_body->uncompressed_body_buffer);
    sv = maybe_message_body->remaining;
    auto body_view = maybe_message_body->body_view;

    logger->trace("decoding download message. "
                  "{download: {server: %1, client: %2} upload: {server: %3, client: %4}, latest: %5}",
                  ret.progress.download.server_version, ret.progress.download.last_integrated_client_version,
                  ret.progress.upload.last_integrated_server_version, ret.progress.upload.client_version,
                  ret.latest_server_version.version);

    while (!body_view.empty()) {
        realm::sync::Transformer::RemoteChangeset cur_changeset;
        std::size_t changeset_size;
        std::tie(body_view, parse_ok) =
            parse_header_line(body_view, ' ', cur_changeset.remote_version,
                              cur_changeset.last_integrated_local_version, cur_changeset.origin_timestamp,
                              cur_changeset.origin_file_ident, cur_changeset.original_changeset_size, changeset_size);
        if (!parse_ok || changeset_size > body_view.size()) {
            logger->error("changeset length is %1 but buffer size is %1", changeset_size, body_view.size());
            return std::nullopt;
        }

        realm::sync::Changeset parsed_changeset;
        realm::_impl::SimpleNoCopyInputStream changeset_stream(body_view.data(), changeset_size);
        realm::sync::parse_changeset(changeset_stream, parsed_changeset);
        logger->trace("found download changeset: serverVersion: %1, clientVersion: %2, origin: %3 %4",
                      cur_changeset.remote_version, cur_changeset.last_integrated_local_version,
                      cur_changeset.origin_file_ident, parsed_changeset);
        realm::BinaryData changeset_data{body_view.data(), changeset_size};
        cur_changeset.data = changeset_data;
        ret.changesets.push_back(cur_changeset);
        body_view = body_view.substr(changeset_size);
    }

    return std::make_pair(std::move(ret), sv);
}

std::optional<std::pair<UploadMessage, StringView>> UploadMessage::parse(StringView sv, Logger* logger)
{
    UploadMessage ret;
    int is_body_compressed;
    std::size_t uncompressed_body_size, compressed_body_size;
    bool parse_ok;

    std::tie(sv, parse_ok) =
        parse_header_line(sv, '\n', ret.session_ident, is_body_compressed, uncompressed_body_size,
                          compressed_body_size, ret.upload_progress.client_version,
                          ret.upload_progress.last_integrated_server_version, ret.locked_server_version);
    if (!parse_ok) {
        return std::nullopt;
    }

    auto maybe_message_body =
        MessageBody::parse(sv, compressed_body_size, uncompressed_body_size, is_body_compressed, logger);
    if (!maybe_message_body) {
        return std::nullopt;
    }
    ret.uncompressed_body_buffer = std::move(maybe_message_body->uncompressed_body_buffer);
    sv = maybe_message_body->remaining;
    auto body_view = maybe_message_body->body_view;

    while (!body_view.empty()) {
        realm::sync::Changeset cur_changeset;
        std::size_t changeset_size;
        std::tie(body_view, parse_ok) =
            parse_header_line(body_view, ' ', cur_changeset.version, cur_changeset.last_integrated_remote_version,
                              cur_changeset.origin_timestamp, cur_changeset.origin_file_ident, changeset_size);
        if (!parse_ok || changeset_size > body_view.size()) {
            return std::nullopt;
        }

        logger->trace("found upload changeset: %1 %2 %3 %4 %5", cur_changeset.last_integrated_remote_version,
                      cur_changeset.version, cur_changeset.origin_timestamp, cur_changeset.origin_file_ident,
                      changeset_size);
        realm::_impl::SimpleNoCopyInputStream changeset_stream(body_view.data(), changeset_size);
        try {
            realm::sync::parse_changeset(changeset_stream, cur_changeset);
        }
        catch (...) {
            logger->error("error decoding changeset after instructions %1", cur_changeset);
            throw;
        }
        logger->trace("Decoded changeset: %1", cur_changeset);
        ret.changesets.push_back(std::move(cur_changeset));
        body_view = body_view.substr(changeset_size);
    }

    return std::make_pair(std::move(ret), sv);
}

void print_usage(StringView program_name)
{
    std::cout << "Synopsis: " << program_name << " -r <PATH-TO-REALM> -i <PATH-TO-MESSAGES> [OPTIONS]"
              << "\n"
                 "Options:\n"
                 "  -h, --help           Display command-line synopsis followed by the list of\n"
                 "                       available options.\n"
                 "  -e, --encryption-key  The file-system path of a file containing a 64-byte\n"
                 "                       encryption key to be used for accessing the specified\n"
                 "                       Realm file.\n"
                 "  -r, --realm          The file-system path to the realm to be created and/or have\n"
                 "                       state applied to.\n"
                 "  -i, --input          The file-system path a file containing UPLOAD, DOWNLOAD,\n"
                 "                       and IDENT messages to apply to the realm state\n"
                 "  --verbose            Print all messages including trace messages to stderr\n"
                 "  -v, --version        Show the version of the Realm Sync release that this\n"
                 "                       command belongs to." << std::endl;
}

int main(int argc, const char** argv)
{
    CliArgumentParser arg_parser;
    CliFlag help_arg(arg_parser, "help", 'h');
    CliArgument realm_arg(arg_parser, "realm", 'r');
    CliArgument encryption_key_arg(arg_parser, "encryption-key", 'e');
    CliArgument input_arg(arg_parser, "input", 'i');
    CliFlag verbose_arg(arg_parser, "verbose");
    auto arg_results = arg_parser.parse(argc, argv);

    std::unique_ptr<RootLogger> logger = std::make_unique<StderrLogger>(); // Throws
    if (verbose_arg) {
        logger->set_level_threshold(Logger::Level::all);
    }
    else {
        logger->set_level_threshold(Logger::Level::error);
    }

    if (help_arg) {
        print_usage(arg_results.program_name);
        return EXIT_SUCCESS;
    }

    if (!realm_arg) {
        logger->error("missing path to realm to apply changesets to");
        print_usage(arg_results.program_name);
        return EXIT_FAILURE;
    }
    if (!input_arg) {
        logger->error("missing path to messages to apply to realm");
        print_usage(arg_results.program_name);
        return EXIT_FAILURE;
    }
    auto realm_path = realm_arg.as<std::string>();

    std::string encryption_key;
    if (encryption_key_arg) {
        encryption_key = load_file(encryption_key_arg.as<std::string>());
    }

    realm::DBOptions dbOpts(encryption_key.empty() ? nullptr : encryption_key.c_str());
    realm::_impl::ClientHistoryImpl history{realm_path};
    auto localDB = realm::DB::create(history, dbOpts);

    auto input_contents = load_file(input_arg.as<std::string>());
    auto input_view = StringView{input_contents};
    while (!input_view.empty()) {
        auto maybe_message = parse_message(input_view, logger.get());
        if (!maybe_message) {
            logger->error("could not find message in input file");
            return EXIT_FAILURE;
        }
        input_view = maybe_message->second;
        if (std::holds_alternative<DownloadMessage>(maybe_message->first)) {
            const auto& download_message = std::get<DownloadMessage>(maybe_message->first);

            realm::sync::VersionInfo version_info;
            realm::sync::ClientReplication::IntegrationError integration_error;
            history.integrate_server_changesets(
                download_message.progress, &download_message.downloadable_bytes, download_message.changesets.data(),
                download_message.changesets.size(), version_info, integration_error, *logger, nullptr);
        }
        if (std::holds_alternative<UploadMessage>(maybe_message->first)) {
            const auto& upload_message = std::get<UploadMessage>(maybe_message->first);

            for (const auto& changeset : upload_message.changesets) {
                auto transaction = localDB->start_write();
                realm::sync::InstructionApplier applier(*transaction);
                applier.apply(changeset, logger.get());
                auto generated_version = transaction->commit();
                logger->debug("integrated local changesets as version %1", generated_version);
            }
        }
        if (std::holds_alternative<ServerIdentMessage>(maybe_message->first)) {
            const auto& ident_message = std::get<ServerIdentMessage>(maybe_message->first);
            history.set_client_file_ident(ident_message.file_ident, true);
        }
    }

    return EXIT_SUCCESS;
}
