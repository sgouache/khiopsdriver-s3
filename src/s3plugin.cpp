#ifdef __CYGWIN__
#define _CRT_SECURE_NO_WARNINGS
#endif

#include "s3plugin.h"
#include "s3plugin_internal.h"
#include "contrib/matching.h"
#include "contrib/ini.h"

#include "spdlog/spdlog.h"

#include <aws/core/Aws.h>
#include <aws/core/auth/AWSCredentials.h>
#include <aws/core/auth/AWSCredentialsProvider.h>
#include <aws/core/utils/stream/PreallocatedStreamBuf.h>
#include <aws/s3/S3Client.h>
#include <aws/s3/model/AbortMultipartUploadRequest.h>
#include <aws/s3/model/CompleteMultipartUploadRequest.h>
#include <aws/s3/model/CreateMultipartUploadRequest.h>
#include <aws/s3/model/DeleteObjectRequest.h>
#include <aws/s3/model/GetObjectRequest.h>
#include <aws/s3/model/HeadObjectRequest.h>
#include <aws/s3/model/ListObjectsV2Request.h>
#include <aws/s3/model/PutObjectRequest.h>
#include <aws/s3/model/UploadPartCopyRequest.h>
#include <aws/s3/model/UploadPartRequest.h>

#include <algorithm>
#include <assert.h>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <limits>
#include <memory>
#include <sstream>
#include <string>
#include <aws/core/utils/logging/DefaultLogSystem.h>
#include <aws/core/utils/logging/ConsoleLogSystem.h>

using namespace Aws::Utils::Logging;
using namespace s3plugin;

using S3Object = Aws::S3::Model::Object;

int bIsConnected = false;

constexpr const char* KHIOPS_S3 = "KHIOPS_S3";

Aws::SDKOptions options;
Aws::UniquePtr<Aws::S3::S3Client> client;

// Global bucket name
Aws::String globalBucketName = "";

HandleContainer<ReaderPtr> active_reader_handles;
HandleContainer<WriterPtr> active_writer_handles;

Aws::String last_error;

constexpr const char* nullptr_msg_stub = "Error passing null pointer to ";

// test utilities

void test_setClient(Aws::UniquePtr<Aws::S3::S3Client>&& mock_client_ptr)
{
	client = std::move(mock_client_ptr);
	bIsConnected = kTrue;
}

void test_unsetClient()
{
	client.reset();
	bIsConnected = kFalse;
}

void test_clearHandles()
{
	active_reader_handles.clear();
	active_writer_handles.clear();
}

void test_cleanupClient()
{
	test_clearHandles();
	test_unsetClient();
}

void* test_getActiveReaderHandles()
{
	return &active_reader_handles;
}

void* test_getActiveWriterHandles()
{
	return &active_writer_handles;
}

#define KH_S3_NOT_CONNECTED(err_val)                                                                                   \
	if (kFalse == bIsConnected)                                                                                    \
	{                                                                                                              \
		LogError("Error: Driver is not connected.");                                                           \
		return (err_val);                                                                                      \
	}

#define ERROR_ON_NULL_ARG(arg, err_val)                                                                                \
	if (!(arg))                                                                                                    \
	{                                                                                                              \
		Aws::OStringStream os;                                                                                 \
		os << nullptr_msg_stub << __func__;                                                                    \
		LogError(os.str());                                                                                    \
		return (err_val);                                                                                      \
	}

#define FIND_HANDLE_OR_ERROR(container, stream, errval)                                                                \
	auto stream##_it = FindHandle((container), (stream));                                                          \
	if (stream##_it == container.end())                                                                            \
	{                                                                                                              \
		LogError("Cannot identify stream");                                                                    \
		return (errval);                                                                                       \
	}                                                                                                              \
	auto& h_ptr = *stream##_it;

#define IF_ERROR(outcome) if (!(outcome).IsSuccess())

#define RETURN_OUTCOME_ON_ERROR(outcome)                                                                               \
	IF_ERROR((outcome))                                                                                            \
	{                                                                                                              \
		return {MakeSimpleError((outcome).GetError())};                                                        \
	}

#define PASS_OUTCOME_ON_ERROR(outcome)                                                                                 \
	IF_ERROR((outcome))                                                                                            \
	{                                                                                                              \
		return (outcome).GetError();                                                                           \
	}

#define RETURN_ON_ERROR(outcome, msg, err_val)                                                                         \
	{                                                                                                              \
		IF_ERROR((outcome))                                                                                    \
		{                                                                                                      \
			LogBadOutcome((outcome), (msg));                                                               \
			return (err_val);                                                                              \
		}                                                                                                      \
	}

#define ERROR_ON_NAMES(status_or_names, err_val) RETURN_ON_ERROR((status_or_names), "Error parsing URL", (err_val))

#define NAMES_OR_ERROR(arg, err_val)                                                                                   \
	auto maybe_parsed_names = ParseS3Uri((arg));                                                                   \
	ERROR_ON_NAMES(maybe_parsed_names, (err_val));                                                                 \
	auto& names = maybe_parsed_names.GetResult();

void LogError(const Aws::String& msg)
{
	spdlog::error(msg);
	last_error = std::move(msg);
}

template <typename R, typename E> void LogBadOutcome(const Aws::Utils::Outcome<R, E>& outcome, const Aws::String& msg)
{
	Aws::OStringStream os;
	os << msg << ": " << outcome.GetError().GetMessage();
	LogError(os.str());
}

template <typename RequestType> RequestType MakeBaseRequest(const Aws::String& bucket, const Aws::String& object)
{
	RequestType request;
	request.WithBucket(bucket).WithKey(object);
	return request;
}

Aws::S3::Model::HeadObjectRequest MakeHeadObjectRequest(const Aws::String& bucket, const Aws::String& object)
{
	return MakeBaseRequest<Aws::S3::Model::HeadObjectRequest>(bucket, object);
}

Aws::S3::Model::GetObjectRequest MakeGetObjectRequest(const Aws::String& bucket, const Aws::String& object,
						      Aws::String&& range = "")
{
	auto request = MakeBaseRequest<Aws::S3::Model::GetObjectRequest>(bucket, object);
	if (!range.empty())
	{
		request.SetRange(std::move(range));
	}
	return request;
}

Aws::S3::Model::GetObjectOutcome GetObject(const Aws::String& bucket, const Aws::String& object,
					   Aws::String&& range = "")
{
	return client->GetObject(MakeGetObjectRequest(bucket, object, std::move(range)));
}

Aws::S3::Model::HeadObjectOutcome HeadObject(const Aws::String& bucket, const Aws::String& object)
{
	return client->HeadObject(MakeHeadObjectRequest(bucket, object));
}

template <typename H> HandleIt<H> FindHandle(HandleContainer<H>& container, void* handle)
{
	return std::find_if(container.begin(), container.end(),
			    [handle](const H& h) { return handle == static_cast<void*>(h.get()); });
}

template <typename H> void EraseRemove(HandleContainer<H>& container, HandleIt<H> pos)
{
	*pos = std::move(container.back());
	container.pop_back();
}

struct SimpleError
{
	int code_;
	Aws::String err_msg_;

	Aws::String GetMessage() const
	{
		return std::to_string(code_) + err_msg_;
	}
};

SimpleError MakeSimpleError(Aws::S3::S3Errors err_code, Aws::String&& err_msg)
{
	return {static_cast<int>(err_code), std::move(err_msg)};
}

SimpleError MakeSimpleError(Aws::S3::S3Errors err_code, const char* err_msg)
{
	return {static_cast<int>(err_code), err_msg};
}

SimpleError MakeSimpleError(const Aws::S3::S3Error& from)
{
	return {static_cast<int>(from.GetErrorType()), from.GetMessage()};
}

struct ParseUriResult
{
	Aws::String bucket_;
	Aws::String object_;
};

using ObjectsVec = Aws::Vector<S3Object>;

template <typename R> using SimpleOutcome = Aws::Utils::Outcome<R, SimpleError>;

using ParseURIOutcome = SimpleOutcome<ParseUriResult>;
using SizeOutcome = SimpleOutcome<long long>;
using FilterOutcome = SimpleOutcome<ObjectsVec>;
using UploadOutcome = SimpleOutcome<bool>; // R can't be void

// Definition of helper functions
Aws::String MakeByteRange(int64_t start, int64_t end)
{
	Aws::StringStream range;
	range << "bytes=" << start << '-' << end;
	return range.str();
}


SizeOutcome DownloadFileRangeToVector(const Aws::String& bucket, const Aws::String& object_name, Aws::Vector<unsigned char>& contentVector,
					std::int64_t start_range, std::int64_t end_range)
{
	// Note: AWS byte ranges are inclusive
	auto request = MakeGetObjectRequest(bucket, object_name, MakeByteRange(start_range, end_range));
	auto outcome = client->GetObject(request);
	RETURN_OUTCOME_ON_ERROR(outcome);

	Aws::IOStream& objectStream = outcome.GetResult().GetBody();
	std::string objectData((std::istreambuf_iterator<char>(objectStream)), std::istreambuf_iterator<char>());

	// Convert string to vector<char>
	contentVector.assign(objectData.begin(), objectData.end());
	return static_cast<long long>(objectData.size());
}

SizeOutcome DownloadFileRangeToBuffer(const Aws::String& bucket, const Aws::String& object_name, unsigned char* buffer,
				      std::int64_t start_range, std::int64_t end_range)
{
	// Note: AWS byte ranges are inclusive
	auto request = MakeGetObjectRequest(bucket, object_name, MakeByteRange(start_range, end_range));
	auto outcome = client->GetObject(request);
	RETURN_OUTCOME_ON_ERROR(outcome);

	// get ownership of the result and its underlying stream
	Aws::S3::Model::GetObjectResult result{outcome.GetResultWithOwnership()};
	auto& stream = result.GetBody();
	// remember comment above about inclusive byte ranges
	stream.read(reinterpret_cast<char*>(buffer), end_range - start_range + 1);

	if (stream.bad())
	{
		return MakeSimpleError(Aws::S3::S3Errors::INTERNAL_FAILURE, "Failed to read stream content");
	}

	return stream.gcount();
}

SizeOutcome ReadBytesInFile(MultiPartFile& multifile, unsigned char* buffer, tOffset to_read)
{
	// Start at first usable file chunk
	// Advance through file chunks, advancing buffer pointer
	// Until last requested byte was read
	// Or error occured

	tOffset bytes_read{0};

	// Lookup item containing initial bytes at requested offset
	const auto& cumul_sizes = multifile.cumulative_sizes_;
	const tOffset common_header_length = multifile.common_header_length_;
	const Aws::String& bucket_name = multifile.bucketname_;
	const auto& filenames = multifile.filenames_;
	unsigned char* buffer_pos = buffer;
	tOffset& offset = multifile.offset_;
	const tOffset offset_bak = offset; // in case of irrecoverable error, leave the multifile in its starting state

	auto greater_than_offset_it = std::upper_bound(cumul_sizes.begin(), cumul_sizes.end(), offset);
	size_t idx = static_cast<size_t>(std::distance(cumul_sizes.begin(), greater_than_offset_it));

	spdlog::debug("Use item {} to read @ {} (end = {})", idx, offset, *greater_than_offset_it);

	auto read_range_and_update = [&](const Aws::String& filename, tOffset start, tOffset end) -> SizeOutcome
	{
		auto download_outcome = DownloadFileRangeToBuffer(
		    bucket_name, filename, buffer_pos, static_cast<int64_t>(start), static_cast<int64_t>(end));
		if (!download_outcome.IsSuccess())
		{
			offset = offset_bak;
			return download_outcome.GetError();
		}

		tOffset actual_read = download_outcome.GetResult();

		spdlog::debug("read = {}", actual_read);

		bytes_read += actual_read;
		buffer_pos += actual_read;
		offset += actual_read;

		if (actual_read < (end - start + 1) /*expected read*/)
		{
			spdlog::debug("End of file encountered");
			to_read = 0;
		}
		else
		{
			to_read -= actual_read;
		}

		return actual_read;
	};

	// first file read

	// AWS peculiarity: byte ranges are inclusive
	const tOffset file_start = (idx == 0) ? offset : offset - cumul_sizes[idx - 1] + common_header_length;
	const tOffset read_end = std::min(file_start + to_read, file_start + cumul_sizes[idx] - offset) - 1;

	SizeOutcome read_outcome = read_range_and_update(filenames[idx], file_start, read_end);

	// continue with the next files
	while (read_outcome.IsSuccess() && to_read)
	{
		// read the missing bytes in the next files as necessary
		idx++;
		const tOffset start = common_header_length;
		const tOffset end = std::min(start + to_read, start + cumul_sizes[idx] - cumul_sizes[idx - 1]) - 1;

		read_outcome = read_range_and_update(filenames[idx], start, end);
	}

	if (read_outcome.IsSuccess())
	{
		read_outcome.GetResult() = bytes_read;
	}

	return read_outcome;
}

// bool UploadBuffer(const Aws::String& bucket_name, const Aws::String& object_name, const char* buffer,
// 		  std::size_t buffer_size)
// {

// 	Aws::S3::Model::PutObjectRequest request;
// 	request.SetBucket(bucket_name);
// 	request.SetKey(object_name);

// 	const std::shared_ptr<Aws::IOStream> inputData = Aws::MakeShared<Aws::StringStream>("");
// 	std::string data;
// 	data.assign(buffer, buffer_size);
// 	*inputData << data.c_str();

// 	request.SetBody(inputData);

// 	Aws::S3::Model::PutObjectOutcome outcome = client->PutObject(request);

// 	if (!outcome.IsSuccess())
// 	{
// 		spdlog::error("PutObjectBuffer: {}", outcome.GetError().GetMessage());
// 	}

// 	return outcome.IsSuccess();
// }

ParseURIOutcome ParseS3Uri(const Aws::String& s3_uri)
{ //, std::string &bucket_name, std::string &object_name) {
	const Aws::String prefix = "s3://";
	const size_t prefix_size = prefix.size();
	if (s3_uri.compare(0, prefix_size, prefix) != 0)
	{
		return SimpleError{static_cast<int>(Aws::S3::S3Errors::INVALID_PARAMETER_VALUE),
				   "Invalid S3 URI: " + s3_uri};
		//{"", "", "Invalid S3 URI: " + s3_uri};
		// spdlog::error("Invalid S3 URI: {}", s3_uri);
		// return false;
	}

	size_t pos = s3_uri.find('/', prefix_size);
	if (pos == std::string::npos)
	{
		return SimpleError{static_cast<int>(Aws::S3::S3Errors::INVALID_PARAMETER_VALUE),
				   "Invalid S3 URI, missing object name: " + s3_uri};
		//{"", "", "Invalid S3 URI, missing object name: " +
		// s3_uri};
		// spdlog::error("Invalid S3 URI, missing object name: {}", s3_uri);
		// return false;
	}

	Aws::String bucket_name = s3_uri.substr(prefix_size, pos - prefix_size);

	if (bucket_name.empty())
	{
		if (globalBucketName.empty())
		{
			return SimpleError{static_cast<int>(Aws::S3::S3Errors::MISSING_PARAMETER),
					   "No bucket specified, and GCS_BUCKET_NAME is not set!"};
		}
		bucket_name = globalBucketName;
	}

	Aws::String object_name = s3_uri.substr(pos + 1);

	return ParseUriResult{std::move(bucket_name), std::move(object_name)};
}

// void FallbackToDefaultBucket(std::string &bucket_name) {
//   if (!bucket_name.empty())
//     return;
//   if (!globalBucketName.empty()) {
//     bucket_name = globalBucketName;
//     return;
//   }
//   spdlog::critical("No bucket specified, and GCS_BUCKET_NAME is not set!");
// }

Aws::String GetEnvironmentVariableOrDefault(const Aws::String& variable_name, const Aws::String& default_value)
{
#ifdef _WIN32
  size_t len;
  char value[2048];
  getenv_s(&len, value, 2048, variable_name.c_str());
#else
  char *value = getenv(variable_name.c_str());
#endif

  if (value && std::strlen(value) > 0) {
    return value;
  } else {
	return default_value;
  }
}

bool IsMultifile(const Aws::String& pattern, size_t& first_special_char_idx)
{
	spdlog::debug("Parse multifile pattern {}", pattern);

	constexpr auto special_chars = "*?![^";

	size_t from_offset = 0;
	size_t found_at = pattern.find_first_of(special_chars, from_offset);
	while (found_at != std::string::npos)
	{
		const char found = pattern[found_at];
		spdlog::debug("special char {} found at {}", found, found_at);

		if (found_at > 0 && pattern[found_at - 1] == '\\')
		{
			spdlog::debug("preceded by a \\, so not so special");
			from_offset = found_at + 1;
			found_at = pattern.find_first_of(special_chars, from_offset);
		}
		else
		{
			spdlog::debug("not preceded by a \\, so really a special char");
			first_special_char_idx = found_at;
			return true;
		}
	}
	return false;
}

// func isMultifile(p string) int {
// 	globalIdx := 0
// 	for {
// 		log.Debugln("Parse multifile pattern", p)
// 		idxChar := strings.IndexAny(p, "*?![^")
// 		if idxChar >= 0 {
// 			log.Debugln("  special char", string([]rune(p)[idxChar]), "found at", globalIdx+idxChar)
// 			globalIdx += idxChar
// 			if idxChar > 0 && string([]rune(p)[idxChar-1]) == "\\" {
// 				log.Debugln("  preceded by a \\, so not so special...")
// 				p = trimLeftChars(p, idxChar+1)
// 			} else {
// 				log.Debugln("  not preceded by a \\, so really a special char...")
// 				return globalIdx
// 			}
// 		}
// 		if idxChar == -1 {
// 			break
// 		}
// 	}
// 	return -1
// }

Aws::S3::Model::ListObjectsV2Outcome ListObjects(const Aws::String& bucket, const Aws::String& pattern)
{
	Aws::S3::Model::ListObjectsV2Request request;
	request.WithBucket(bucket).WithPrefix(pattern).WithDelimiter("");
	return client->ListObjectsV2(request);
}

// Get from a bucket a list of objects matching a name pattern.
// To get a limited list of objects to filter per request, the request includes a well defined
// prefix contained in the pattern
FilterOutcome FilterList(const Aws::String& bucket, const Aws::String& pattern, size_t pattern_1st_sp_char_pos)
{
	ObjectsVec res;

	Aws::S3::Model::ListObjectsV2Request request;
	request.WithBucket(bucket).WithPrefix(pattern.substr(0, pattern_1st_sp_char_pos)); //.WithDelimiter("");
	Aws::String continuation_token;

	do
	{
		if (!continuation_token.empty())
		{
			request.SetContinuationToken(continuation_token);
		}
		const Aws::S3::Model::ListObjectsV2Outcome outcome = client->ListObjectsV2(request);

		RETURN_OUTCOME_ON_ERROR(outcome);

		const auto& list_result = outcome.GetResult();
		const auto& objects = list_result.GetContents();
		std::copy_if(objects.begin(), objects.end(), std::back_inserter(res),
			     [&](const S3Object& obj) { return utils::gitignore_glob_match(obj.GetKey(), pattern); });
		continuation_token = list_result.GetContinuationToken();

	} while (!continuation_token.empty());

	return res;
}

#define KH_S3_FILTER_LIST(var, bucket, pattern, pattern_1st_sp_char_pos)                                               \
	const auto var##_outcome = FilterList(bucket, pattern, pattern_1st_sp_char_pos);                               \
	PASS_OUTCOME_ON_ERROR(var##_outcome);                                                                          \
	const ObjectsVec& var = var##_outcome.GetResult();

#define KH_S3_EMPTY_LIST(list)                                                                                         \
	if ((list).empty())                                                                                            \
	{                                                                                                              \
		return MakeSimpleError(Aws::S3::S3Errors::RESOURCE_NOT_FOUND, "No match for the file pattern");        \
	}

bool WillSizeCountProductOverflow(size_t size, size_t count)
{
	constexpr size_t max_prod_usable{static_cast<size_t>(std::numeric_limits<tOffset>::max())};
	return (max_prod_usable / size < count || max_prod_usable / count < size);
}

template <typename Request> Request MakeBaseUploadRequest(const Writer& writer)
{
	const auto& multipartupload_data = writer.writer_;

	return Request{}
	    .WithBucket(multipartupload_data.GetBucket())
	    .WithKey(multipartupload_data.GetKey())
	    .WithUploadId(multipartupload_data.GetUploadId());
}

template <typename PartRequest> PartRequest MakeBaseUploadPartRequest(const Writer& writer)
{
	return MakeBaseUploadRequest<PartRequest>(writer).WithPartNumber(writer.part_tracker_);
}

Aws::S3::Model::UploadPartRequest MakeUploadPartRequest(Writer& writer,
							Aws::Utils::Stream::PreallocatedStreamBuf& pre_buf)
{
	Aws::S3::Model::UploadPartRequest request =
	    MakeBaseUploadPartRequest<Aws::S3::Model::UploadPartRequest>(writer);

	const auto body = Aws::MakeShared<Aws::IOStream>(KHIOPS_S3, &pre_buf);
	request.SetBody(body);
	return request;
}

Aws::S3::Model::UploadPartCopyRequest MakeUploadPartCopyRequest(Writer& writer, const Aws::String& byte_range)
{
	return MakeBaseUploadPartRequest<Aws::S3::Model::UploadPartCopyRequest>(writer)
	    .WithCopySource(writer.append_target_)
	    .WithCopySourceRange(byte_range);
}

Aws::S3::Model::CompleteMultipartUploadRequest MakeCompleteMultipartUploadRequest(Writer& writer)
{
	Aws::S3::Model::CompletedMultipartUpload request_body;
	request_body.SetParts(writer.parts_);

	return MakeBaseUploadRequest<Aws::S3::Model::CompleteMultipartUploadRequest>(writer).WithMultipartUpload(
	    std::move(request_body));
}

// Implementation of driver functions

const char* driver_getDriverName()
{
	return "S3 driver";
}

const char* driver_getVersion()
{
	return "0.1.0";
}

const char* driver_getScheme()
{
	return "s3";
}

int driver_isReadOnly()
{
	return 0;
}

int driver_connect()
{
	if (kTrue == bIsConnected)
	{
		spdlog::debug("Driver is already connected");
		return kSuccess;
	}

	auto file_exists = [](const Aws::String& name)
	{
		Aws::IFStream ifile(name);
		return (ifile.is_open());
	};

	const auto loglevel = GetEnvironmentVariableOrDefault("S3_DRIVER_LOGLEVEL", "info");
	if (loglevel == "debug")
		spdlog::set_level(spdlog::level::debug);
	else if (loglevel == "trace")
		spdlog::set_level(spdlog::level::trace);
	else
		spdlog::set_level(spdlog::level::info);

	spdlog::debug("Connect {}", loglevel);

	// Configuration: we honor both standard AWS config files and environment
	// variables If both configuration files and environment variables are set
	// precedence is given to environment variables
	Aws::String s3endpoint = "";
	Aws::String s3region = "us-east-1";

	// Note: this might be useless now since AWS SDK apparently allows setting 
	// custom endpoints now...

	// Load AWS configuration from file
	Aws::Auth::AWSCredentials configCredentials;
	Aws::String userHome = GetEnvironmentVariableOrDefault("HOME", "");
	if (!userHome.empty())
	{
		Aws::OStringStream defaultConfig_os;
		defaultConfig_os << userHome << "/.aws/config";
		const std::string defaultConfig = defaultConfig_os.str();

		// std::string defaultConfig = std::filesystem::path(userHome)
		//                                 .append(".aws")
		//                                 .append("config")
		//                                 .string();

		const Aws::String configFile = GetEnvironmentVariableOrDefault("AWS_CONFIG_FILE", defaultConfig);
		spdlog::debug("Conf file = {}", configFile);

		if (file_exists(configFile))
		{
			// if (std::filesystem::exists(std::filesystem::path(configFile))) {
			const Aws::String profile = GetEnvironmentVariableOrDefault("AWS_PROFILE", "default");

			spdlog::debug("Profile = {}", profile);

			const Aws::String profileSection = (profile != "default") ? "profile " + profile : profile;

			Aws::Auth::ProfileConfigFileAWSCredentialsProvider provider(profile.c_str());
			configCredentials = provider.GetAWSCredentials();

			mINI::INIFile file(configFile);
			mINI::INIStructure ini;
			file.read(ini);
			Aws::String confEndpoint = ini.get(profileSection).get("endpoint_url");
			if (!confEndpoint.empty())
			{
				s3endpoint = std::move(confEndpoint);
			}
			spdlog::debug("Endpoint = {}", s3endpoint);

			Aws::String confRegion = ini.get(profileSection).get("region");
			if (!confRegion.empty())
			{
				s3region = std::move(confRegion);
			}
			spdlog::debug("Region = {}", s3region);
		}
		else if (configFile != defaultConfig)
		{
			return kFailure;
		}
	}

	// Initialize variables from environment
	// Both AWS_xxx standard variables and AutoML S3_xxx variables are supported
	// If both are present, AWS_xxx variables will be given precedence

	// Note: this behavior is normally the same as the one implemented by the SDK
	// except for the "S3_*" variables that are kept to support legacy applications
	
	globalBucketName = GetEnvironmentVariableOrDefault("S3_BUCKET_NAME", "");
	s3endpoint = GetEnvironmentVariableOrDefault("S3_ENDPOINT", s3endpoint);
	s3endpoint = GetEnvironmentVariableOrDefault("AWS_ENDPOINT_URL", s3endpoint);
	s3region = GetEnvironmentVariableOrDefault("AWS_DEFAULT_REGION", s3region);
	Aws::String s3accessKey = GetEnvironmentVariableOrDefault("S3_ACCESS_KEY", "");
	s3accessKey = GetEnvironmentVariableOrDefault("AWS_ACCESS_KEY_ID", s3accessKey);
	Aws::String s3secretKey = GetEnvironmentVariableOrDefault("S3_SECRET_KEY", "");
	s3secretKey = GetEnvironmentVariableOrDefault("AWS_SECRET_ACCESS_KEY", s3secretKey);
	if ((s3accessKey != "" && s3secretKey == "") || (s3accessKey == "" && s3secretKey != ""))
	{
		LogError("Access key and secret configuration is only permitted "
			 "when both values are provided.");
		return false;
	}

	if (!GetEnvironmentVariableOrDefault("AWS_DEBUG_HTTP_LOGS", "").empty()) {
		options.loggingOptions.logLevel = Aws::Utils::Logging::LogLevel::Debug;
		options.loggingOptions.logger_create_fn = [] { return std::make_shared<ConsoleLogSystem>(LogLevel::Debug); };
	}

	// Initialisation du SDK AWS
	Aws::InitAPI(options);

	Aws::Client::ClientConfiguration clientConfig(true, "legacy", true);
	clientConfig.allowSystemProxy = !GetEnvironmentVariableOrDefault("http_proxy", "").empty() || 
	!GetEnvironmentVariableOrDefault("https_proxy", "").empty() ||
		!GetEnvironmentVariableOrDefault("HTTP_PROXY", "").empty() || 
		!GetEnvironmentVariableOrDefault("HTTPS_PROXY", "").empty() || 
		!GetEnvironmentVariableOrDefault("S3_ALLOW_SYSTEM_PROXY", "").empty();
	clientConfig.verifySSL = true;
	clientConfig.version = Aws::Http::Version::HTTP_VERSION_2TLS;
	if (s3endpoint != "")
	{
		clientConfig.endpointOverride = std::move(s3endpoint);
	}
	if (s3region != "")
	{
		clientConfig.region = s3region;
	}

	if (!s3accessKey.empty())
	{
		configCredentials = Aws::Auth::AWSCredentials(s3accessKey, s3secretKey);
	}

	client = Aws::MakeUnique<Aws::S3::S3Client>(KHIOPS_S3, configCredentials, 
							Aws::MakeShared<Aws::S3::S3EndpointProvider>(KHIOPS_S3),
						    clientConfig);

	bIsConnected = true;
	return kSuccess;
}

int driver_disconnect()
{
	if (client)
	{
		// tie up loose ends
		Aws::Vector<Aws::S3::Model::AbortMultipartUploadOutcome> failures;
		for (auto h_it = active_writer_handles.begin(); h_it != active_writer_handles.end();)
		{
			auto& writer = **h_it;
			auto outcome = client->AbortMultipartUpload(
			    MakeBaseUploadRequest<Aws::S3::Model::AbortMultipartUploadRequest>(writer));

			if (outcome.IsSuccess())
			{
				// delete the handle
				h_it = active_writer_handles.erase(h_it);
			}
			else
			{
				failures.push_back(std::move(outcome));
				h_it++;
			}
		}

		if (!failures.empty())
		{
			Aws::OStringStream os;
			os << "Errors occured during disconnection:\n";
			for (const auto& outcome : failures)
			{
				os << outcome.GetError().GetMessage() << '\n';
			}
			LogError(os.str());

			return kFailure;
		}
	}

	active_writer_handles.clear();
	active_reader_handles.clear();

	client.reset();
	
	//Aws::Utils::Logging::ShutdownAWSLogging();
	ShutdownAPI(options);

	bIsConnected = kFalse;

	return kSuccess;
}

int driver_isConnected()
{
	return bIsConnected;
}

long long int driver_getSystemPreferredBufferSize()
{
	constexpr long long buff_size = 4L * 1024L * 1024L;
	return buff_size; // 4 Mo
}

int driver_exist(const char* filename)
{
	KH_S3_NOT_CONNECTED(kFalse);

	ERROR_ON_NULL_ARG(filename, kFalse);

	const size_t size = std::strlen(filename);
	if (0 == size)
	{
		LogError("Error passing an empty name to driver_exist");
		return kFalse;
	}

	spdlog::debug("exist {}", filename);

	// const std::string file_uri = filename;
	// spdlog::debug("exist file_uri {}", file_uri);
	const char last_char = filename[std::strlen(filename) - 1];
	spdlog::debug("exist last char {}", last_char);

	if (last_char == '/')
	{
		return driver_dirExists(filename);
	}
	else
	{
		return driver_fileExists(filename);
	}
}

int driver_fileExists(const char* sFilePathName)
{
	KH_S3_NOT_CONNECTED(kFalse);

	ERROR_ON_NULL_ARG(sFilePathName, kFalse);

	spdlog::debug("fileExist {}", sFilePathName);

	NAMES_OR_ERROR(sFilePathName, kFalse);

	size_t pattern_1st_sp_char_pos = 0;
	if (!IsMultifile(names.object_, pattern_1st_sp_char_pos))
	{
		//go ahead with the simple request
		const auto head_object_outcome = HeadObject(names.bucket_, names.object_);
		if (head_object_outcome.GetError().GetErrorType() == Aws::S3::S3Errors::RESOURCE_NOT_FOUND)
		{
			return kFalse;
		}
		RETURN_ON_ERROR(head_object_outcome, "Failed retrieving file info in fileExists", kFalse);

		return kTrue;
	}

	// get a filtered list of the bucket files that match the pattern
	auto filter_list_outcome = FilterList(names.bucket_, names.object_, pattern_1st_sp_char_pos);
	RETURN_ON_ERROR(filter_list_outcome, "Error while filtering object list", kFalse);

	return filter_list_outcome.GetResult().empty() ? kFalse : kTrue;
}

int driver_dirExists(const char* sFilePathName)
{
	KH_S3_NOT_CONNECTED(kFalse);

	ERROR_ON_NULL_ARG(sFilePathName, kFalse);

	spdlog::debug("dirExist {}", sFilePathName);

	return kTrue;
}

SizeOutcome GetOneFileSize(const Aws::String& bucket, const Aws::String& object)
{
	const auto head_object_outcome = HeadObject(bucket, object);
	RETURN_OUTCOME_ON_ERROR(head_object_outcome);
	return head_object_outcome.GetResult().GetContentLength();
}

SimpleOutcome<Aws::String> ReadHeader(const Aws::String& bucket, const S3Object& obj)
{
	auto request = MakeGetObjectRequest(bucket, obj.GetKey());
	auto outcome = client->GetObject(request);
	RETURN_OUTCOME_ON_ERROR(outcome);
	auto result = outcome.GetResultWithOwnership();
	Aws::IOStream& read_stream = result.GetBody();
	Aws::String line;
	std::getline(read_stream, line);
	if (read_stream.bad())
	{
		return MakeSimpleError(Aws::S3::S3Errors::INTERNAL_FAILURE, "header read failed");
	}
	if (!read_stream.eof())
	{
		line.push_back('\n');
	}
	if (line.empty())
	{
		return MakeSimpleError(Aws::S3::S3Errors::INTERNAL_FAILURE, "Empty header");
	}
	return line;
}

#define KH_S3_READ_HEADER(var, bucket, obj)                                                                            \
	const auto var##_outcome = ReadHeader((bucket), (obj));                                                        \
	PASS_OUTCOME_ON_ERROR(var##_outcome);                                                                          \
	const Aws::String& var = var##_outcome.GetResult();

SizeOutcome getFileSize(const Aws::String& bucket_name, const Aws::String& object_name)
{
	// tweak the request for the object. if the object parameter is in fact a pattern,
	// the pattern could point to a list of objects that constitute a whole file

	size_t pattern_1st_sp_char_pos = 0;
	if (!IsMultifile(object_name, pattern_1st_sp_char_pos))
	{
		//go ahead with the simple request
		return GetOneFileSize(bucket_name, object_name);
	}

	KH_S3_FILTER_LIST(file_list, bucket_name, object_name,
			  pattern_1st_sp_char_pos); // !! puts file_list and file_list_outcome into scope

	KH_S3_EMPTY_LIST(file_list);

	// get the size of the first file
	const S3Object& first_file = file_list.front();
	long long total_size = first_file.GetSize();

	// special case: one element
	if (file_list.size() == 1)
	{
		return total_size;
	}

	// general case: more than one element
	// read the size of the header

	KH_S3_READ_HEADER(header, bucket_name, first_file); // !! puts header and outcome_header into scope

	const size_t header_size = header.size();

	// scan the next files and adjust effective size if header is repeated
	int nb_headers_to_subtract = 0;
	bool same_header = true;

	for (size_t i = 1; i < file_list.size(); i++)
	{
		const S3Object& curr_file = file_list[i];
		if (same_header)
		{
			KH_S3_READ_HEADER(curr_header, bucket_name,
					  curr_file); // !! puts curr_header_outcome and curr_header into scope

			same_header = (header == curr_header);
			if (same_header)
			{
				nb_headers_to_subtract++;
			}
		}
		total_size += curr_file.GetSize();
	}

	if (!same_header)
	{
		nb_headers_to_subtract = 0;
	}
	return total_size - static_cast<long long>(nb_headers_to_subtract * header_size);
}

long long int driver_getFileSize(const char* filename)
{
	KH_S3_NOT_CONNECTED(kBadSize);

	ERROR_ON_NULL_ARG(filename, kBadSize);

	spdlog::debug("getFileSize {}", filename);

	NAMES_OR_ERROR(filename, kBadSize);
	const auto maybe_file_size = getFileSize(names.bucket_, names.object_);
	RETURN_ON_ERROR(maybe_file_size, "Error getting file size", kBadSize);

	return maybe_file_size.GetResult();
}

SimpleOutcome<ReaderPtr> MakeReaderPtr(Aws::String bucketname, Aws::String objectname)
{
	size_t pattern_1st_sp_char_pos = 0;
	if (!IsMultifile(objectname, pattern_1st_sp_char_pos))
	{
		// create a Multifile with a single file
		const auto size_outcome = GetOneFileSize(bucketname, objectname);
		PASS_OUTCOME_ON_ERROR(size_outcome);
		const long long size = size_outcome.GetResult();

		Aws::Vector<Aws::String> objectnames(1, objectname);
		Aws::Vector<tOffset> sizes(1, size);

		return Aws::MakeUnique<Reader>(KHIOPS_S3, std::move(bucketname), std::move(objectname), 0, 0,
					       std::move(objectnames), std::move(sizes));
	}

	// this is a multifile. the reader object needs the list of filenames matching the globbing pattern and their
	// metadata, mainly their respective sizes.

	// Note: getting the metadata involves a tradeoff between memory size of the data kept and the amount of data copied:
	// storing only the relevant data in the MultiPartFile struct requires another copy of each object name, since the API
	// does not allow moving from its own Object types. These copies could be avoided by keeping the entire list of Objects,
	// at the cost of the space used by the other metadata. The implementation here will save that space.

	KH_S3_FILTER_LIST(file_list, bucketname, objectname,
			  pattern_1st_sp_char_pos); // !! file_list and file_list_outcome now in scope

	KH_S3_EMPTY_LIST(file_list);

	const size_t file_count = file_list.size();
	Aws::Vector<Aws::String> filenames(file_count);
	Aws::Vector<long long> cumulative_size(file_count);

	// get metadata from the first file
	const auto& first_file = file_list.front();
	filenames.front() = first_file.GetKey();
	cumulative_size.front() = first_file.GetSize();
	tOffset common_header_length = 0;

	if (file_count > 1)
	{
		bool same_header = true;

		// more than one file, the headers need to be checked
		KH_S3_READ_HEADER(header, bucketname, first_file); // !! puts header and header_outcome into scope
		tOffset header_length = static_cast<tOffset>(header.size());

		for (size_t i = 1; i < file_count; i++)
		{
			const auto& curr_file = file_list[i];
			filenames[i] = curr_file.GetKey();
			cumulative_size[i] = cumulative_size[i - 1] + curr_file.GetSize();

			if (same_header)
			{
				// continue checking the header of the file:
				KH_S3_READ_HEADER(curr_header, bucketname,
						  curr_file); // !! puts curr_header and curr_header_outcome into scope
				same_header = (curr_header == header);
			}
		}

		// if headers remained the same, adjust the cumulative sizes
		if (same_header)
		{
			common_header_length = header_length;
			for (size_t i = 1; i < file_count; i++)
			{
				cumulative_size[i] -= (i * common_header_length);
			}
		}
	}

	// construct the result
	return Aws::MakeUnique<Reader>(KHIOPS_S3, std::move(bucketname), std::move(objectname), 0,
				       common_header_length, std::move(filenames), std::move(cumulative_size));
}

SimpleOutcome<WriterPtr> MakeWriterPtr(Aws::String bucket, Aws::String object)
{
	Aws::S3::Model::CreateMultipartUploadRequest request;
	request.SetBucket(std::move(bucket));
	request.SetKey(std::move(object));
	auto outcome = client->CreateMultipartUpload(request);
	RETURN_OUTCOME_ON_ERROR(outcome);
	return Aws::MakeUnique<Writer>(KHIOPS_S3, outcome.GetResultWithOwnership());
}

// This template is only here to get specialized
template <typename T> T* PushBackHandle(Aws::UniquePtr<T>&&)
{
	return nullptr;
}

template <> Reader* PushBackHandle<Reader>(ReaderPtr&& stream_ptr)
{
	active_reader_handles.push_back(std::move(stream_ptr));
	return active_reader_handles.back().get();
}

template <> Writer* PushBackHandle<Writer>(WriterPtr&& stream_ptr)
{
	active_writer_handles.push_back(std::move(stream_ptr));
	return active_writer_handles.back().get();
}

template <typename Stream>
SimpleOutcome<Stream*>
RegisterStream(std::function<SimpleOutcome<Aws::UniquePtr<Stream>>(Aws::String, Aws::String)> MakeStreamPtr,
	       Aws::String&& bucket, Aws::String&& object)
{
	auto outcome = MakeStreamPtr(std::move(bucket), std::move(object));
	PASS_OUTCOME_ON_ERROR(outcome);
	return PushBackHandle(outcome.GetResultWithOwnership());
}

SimpleOutcome<Reader*> RegisterReader(Aws::String&& bucket, Aws::String&& object)
{
	return RegisterStream<Reader>(MakeReaderPtr, std::move(bucket), std::move(object));
}

SimpleOutcome<Writer*> RegisterWriter(Aws::String&& bucket, Aws::String&& object)
{
	return RegisterStream<Writer>(MakeWriterPtr, std::move(bucket), std::move(object));
}

#define KH_S3_REGISTER_STREAM(type, bucket, object, err_msg)                                                           \
	auto outcome = Register##type(std::move(bucket), std::move(object));                                           \
	RETURN_ON_ERROR(outcome, err_msg, nullptr);                                                                    \
	return outcome.GetResult();

template <typename Result> void UpdateUploadMetadata(Writer& writer, const Result& result)
{
	Aws::S3::Model::CompletedPart part;
	part.SetETag(result.GetETag());
	part.SetPartNumber(writer.part_tracker_);
	writer.parts_.push_back(std::move(part));
	writer.part_tracker_++;
}

UploadOutcome UploadPart(Writer& writer)
{
	auto& buffer = writer.buffer_;
	Aws::Utils::Stream::PreallocatedStreamBuf pre_buf(buffer.data(), buffer.size());
	const auto request = MakeUploadPartRequest(writer, pre_buf);
	auto outcome = client->UploadPart(request);
	RETURN_OUTCOME_ON_ERROR(outcome);

	UpdateUploadMetadata(writer, outcome.GetResult());

	return true;
}

UploadOutcome UploadPartCopy(Writer& writer, const Aws::String& byte_range)
{
	auto outcome = client->UploadPartCopy(MakeUploadPartCopyRequest(writer, byte_range));
	RETURN_OUTCOME_ON_ERROR(outcome);
	UpdateUploadMetadata(writer, outcome.GetResult().GetCopyPartResult());
	return true;
}

UploadOutcome InitiateAppend(Writer& writer, size_t source_bytes_to_copy)
{
	// Make the requests to copy the source file.
	// If the source file is smaller than 5MB, the source needs to be
	// stored in an internal buffer and wait until more data arrives.
	//
	// Conversely, if the source file exceeds 5GB, the copy will be done
	// by parts. If the last part is smaller than 5MB, the last data range
	// will be copied into the internal buffer and wait there.

	const auto& multipartupload_data = writer.writer_;
	int64_t start_range = 0;
	while (source_bytes_to_copy > Writer::buff_min_)
	{
		const int64_t copy_count =
		    static_cast<int64_t>(source_bytes_to_copy > Writer::buff_max_ ? Writer::buff_max_ : source_bytes_to_copy);
			
		// peculiarity of AWS: the range for the copy request has an inclusive end,
		// meaning that the bytes numbered start_range to end_range included are copied
		const int64_t end_range = start_range + copy_count - 1;
		auto outcome = UploadPartCopy(writer, MakeByteRange(start_range, end_range));
		PASS_OUTCOME_ON_ERROR(outcome);

		source_bytes_to_copy -= static_cast<size_t>(copy_count);
		start_range += copy_count;
	}

	// copy in the internal buffer what remains from the source.
	if (source_bytes_to_copy > 0)
	{
		writer.buffer_.reserve(source_bytes_to_copy);
		// reminder: byte ranges are inclusive
		auto outcome = DownloadFileRangeToVector(multipartupload_data.GetBucket(),
							 multipartupload_data.GetKey(), writer.buffer_,
							 start_range, start_range + static_cast<int64_t>(source_bytes_to_copy) - 1);
		PASS_OUTCOME_ON_ERROR(outcome);

		tOffset actual_read = outcome.GetResult();

		spdlog::debug("copied = {}", actual_read);
	}

	return true;
}

void* driver_fopen(const char* filename, char mode)
{
	KH_S3_NOT_CONNECTED(nullptr);

	ERROR_ON_NULL_ARG(filename, nullptr);

	spdlog::debug("fopen {} {}", filename, mode);

	NAMES_OR_ERROR(filename, nullptr);

	switch (mode)
	{
	case 'r':
	{
		KH_S3_REGISTER_STREAM(Reader, names.bucket_, names.object_, "Error while opening reader stream");
	}
	case 'w':
	{
		KH_S3_REGISTER_STREAM(Writer, names.bucket_, names.object_, "Error while opening writer stream");
	}
	case 'a':
	{
		// identify the concrete target of the append
		Aws::String target;

		size_t pattern_1st_sp_char_pos = 0;
		if (IsMultifile(names.object_, pattern_1st_sp_char_pos))
		{
			const auto file_list_outcome =
			    FilterList(names.bucket_, names.object_, pattern_1st_sp_char_pos);
			RETURN_ON_ERROR(file_list_outcome, "Error while looking for existing file", nullptr);
			const ObjectsVec& file_list = file_list_outcome.GetResult();

			if (!file_list.empty())
			{
				target = file_list.back().GetKey();
			}
			else
			{
				spdlog::debug("No match for the file pattern.");
			}
		}
		else
		{
			target = names.object_;
		}

		// if file does not already exist, fallback to simple write mode
		auto head_outcome = HeadObject(names.bucket_, target);
		if (!head_outcome.IsSuccess())
		{
			auto& error = head_outcome.GetError();
			if (error.GetErrorType() == Aws::S3::S3Errors::NO_SUCH_KEY ||
			    error.GetErrorType() == Aws::S3::S3Errors::RESOURCE_NOT_FOUND)
			{
				// source file not found, fallback to simple write mode
				spdlog::debug("No source file to append to, falling back to simple write.");
				KH_S3_REGISTER_STREAM(Writer, names.bucket_, target,
						      "Error while opening writer stream");
			}
			else
			{
				// genuine error
				LogBadOutcome(head_outcome, "Error while opening append stream");
				return nullptr;
			}
		}

		// file exists, but is immutable. the strategy is to copy the content to a new version of the file,
		// add the new content with writes and complete, deleting the previous version of the file at the end
		// of the process
		// for the opening, gather the origin file metadata and issue the request to copy its parts
		auto register_outcome = RegisterWriter(std::move(names.bucket_), std::move(target));
		RETURN_ON_ERROR(register_outcome, "Error while opening append stream", nullptr);
		auto writer_ptr = register_outcome.GetResult();
		writer_ptr->append_target_ = head_outcome.GetResult().GetVersionId();

		// requests for copy
		const auto init_outcome =
		    InitiateAppend(*writer_ptr, static_cast<size_t>(head_outcome.GetResult().GetContentLength()));
		RETURN_ON_ERROR(init_outcome, "Error while initiating append stream", nullptr);

		return writer_ptr;
	}
	default:
    	LogError(std::string("Invalid open mode: ") + mode);
		return nullptr;
	}
}

#define KH_S3_FIND_AND_REMOVE(type, container, stream)                                                                 \
	auto type##_handle_it = FindHandle((container), (stream));                                                     \
	if (type##_handle_it != (container).end())                                                                     \
	{                                                                                                              \
		EraseRemove((container), type##_handle_it);                                                            \
		return kCloseSuccess;                                                                                  \
	}

int driver_fclose(void* stream)
{
	KH_S3_NOT_CONNECTED(kCloseEOF);

	ERROR_ON_NULL_ARG(stream, kCloseEOF);

	spdlog::debug("fclose {}", (void*)stream);

	KH_S3_FIND_AND_REMOVE(reader, active_reader_handles, stream);

	auto writer_h_it = FindHandle(active_writer_handles, stream);
	if (writer_h_it != active_writer_handles.end())
	{
		// end multipart upload
		// first, flush the pending data
		auto& writer = **writer_h_it;
		const auto upload_outcome = UploadPart(writer);
		RETURN_ON_ERROR(upload_outcome, "Error during upload", kCloseEOF);

		// close upload
		const auto complete_outcome =
		    client->CompleteMultipartUpload(MakeCompleteMultipartUploadRequest(writer));

		// the request can fail and allow retries.
		// if the request fails, the parts are still present on server side!
		// to be able to delete the parts, the writer handle must remain in
		// the list of active handles.
		RETURN_ON_ERROR(complete_outcome, "Error completing upload while closing stream", kCloseEOF);

		EraseRemove(active_writer_handles, writer_h_it);

		return kCloseSuccess;
	}

	LogError("Cannot identify stream");
	return kCloseEOF;
}

int driver_fseek(void* stream, long long int offset, int whence)
{
	KH_S3_NOT_CONNECTED(kBadSize);

	constexpr long long max_val = std::numeric_limits<long long>::max();

	ERROR_ON_NULL_ARG(stream, kBadSize);

	// confirm stream's presence
	FIND_HANDLE_OR_ERROR(active_reader_handles, stream, kBadSize);
	auto& h = *h_ptr;

	// if (HandleType::kRead != stream_h->type)
	// {
	//     LogError("Cannot seek on not reading stream");
	//     return -1;
	// }

	spdlog::debug("fseek {} {} {}", stream, offset, whence);

	// MultiPartFile &h = stream_h->GetReader();

	tOffset computed_offset{0};

	switch (whence)
	{
	case std::ios::beg:
		computed_offset = offset;
		break;
	case std::ios::cur:
		if (offset > max_val - h.offset_)
		{
			LogError("Signed overflow prevented");
			return kBadSize;
		}
		computed_offset = h.offset_ + offset;
		break;
	case std::ios::end:
		if (h.total_size_ > 0)
		{
			long long minus1 = h.total_size_ - 1;
			if (offset > max_val - minus1)
			{
				LogError("Signed overflow prevented");
				return kBadSize;
			}
		}
		if ((offset == std::numeric_limits<long long>::min()) && (h.total_size_ == 0))
		{
			LogError("Signed overflow prevented");
			return kBadSize;
		}

		computed_offset = (h.total_size_ == 0) ? offset : h.total_size_ - 1 + offset;
		break;
	default:
		LogError("Invalid seek mode " + std::to_string(whence));
		return kBadSize;
	}

	if (computed_offset < 0)
	{
		LogError("Invalid seek offset " + std::to_string(computed_offset));
		return kBadSize;
	}
	h.offset_ = computed_offset;
	return 0;
}

const char* driver_getlasterror()
{
	spdlog::debug("getlasterror");

	return NULL;
}

long long int driver_fread(void* ptr, size_t size, size_t count, void* stream)
{
	KH_S3_NOT_CONNECTED(kBadSize);

	ERROR_ON_NULL_ARG(stream, kBadSize);
	ERROR_ON_NULL_ARG(ptr, kBadSize);

	if (0 == size)
	{
		LogError("Error passing size of 0");
		return kBadSize;
	}

	spdlog::debug("fread {} {} {} {}", ptr, size, count, stream);

	// confirm stream's presence
	FIND_HANDLE_OR_ERROR(active_reader_handles, stream, kBadSize);
	auto& h = *h_ptr;

	const tOffset offset = h.offset_;

	// fast exit for 0 read
	if (0 == count)
	{
		return 0;
	}

	// prevent overflow
	if (WillSizeCountProductOverflow(size, count))
	{
		LogError("product size * count is too large, would overflow");
		return kBadSize;
	}

	tOffset to_read{static_cast<tOffset>(size * count)};
	if (offset > std::numeric_limits<long long>::max() - to_read)
	{
		LogError("signed overflow prevented on reading attempt");
		return kBadSize;
	}
	// end of overflow prevention

	// special case: if offset >= total_size, error if not 0 byte required. 0 byte required is already done above
	const tOffset total_size = h.total_size_;
	if (offset >= total_size)
	{
		LogError("Error trying to read more bytes while already out of bounds");
		return kBadSize;
	}

	// normal cases
	if (offset + to_read > total_size)
	{
		to_read = total_size - offset;
		spdlog::debug("offset {}, req len {} exceeds file size ({}) -> reducing len to {}", offset, to_read,
			      total_size, to_read);
	}
	else
	{
		spdlog::debug("offset = {} to_read = {}", offset, to_read);
	}

	auto read_outcome = ReadBytesInFile(h, reinterpret_cast<unsigned char*>(ptr), to_read);
	RETURN_ON_ERROR(read_outcome, "Error while reading from file", kBadSize);

	return read_outcome.GetResult();
}

long long int driver_fwrite(const void* ptr, size_t size, size_t count, void* stream)
{
	KH_S3_NOT_CONNECTED(kBadSize);

	ERROR_ON_NULL_ARG(stream, kBadSize);
	ERROR_ON_NULL_ARG(ptr, kBadSize);

	if (0 == size)
	{
		LogError("Error passing size 0 to fwrite");
		return kBadSize;
	}

	spdlog::debug("fwrite {} {} {} {}", ptr, size, count, stream);

	FIND_HANDLE_OR_ERROR(active_writer_handles, stream, kBadSize);

	// fast exit for 0
	if (0 == count)
	{
		return 0;
	}

	// prevent integer overflow
	if (WillSizeCountProductOverflow(size, count))
	{
		LogError("Error on write: product size * count is too large, would overflow");
		return kBadSize;
	}

	const size_t to_write = size * count;

	// tune up the capacity of the internal buffer, the final buffer size must be a multiple of the size argument
	auto& buffer = h_ptr->buffer_;
	const size_t curr_size = buffer.size();
	const size_t next_size = curr_size + to_write;
	if (next_size > buffer.capacity())
	{
		// if next_size exceeds max capacity, reserve the closest capacity under buff_max_ that is a multiple of size argument,
		// else reserve next_size
		buffer.reserve(next_size > WriteFile::buff_max_ ? (WriteFile::buff_max_ / size) * size : next_size);
	}

	// copy up to capacity or the whole data for now
	size_t remain = to_write;
	const size_t available = buffer.capacity() - buffer.size();
	size_t copy_count = std::min(available, remain);
	const unsigned char* ptr_cast_pos = reinterpret_cast<const unsigned char*>(ptr);

	auto copy_and_update =
	    [](Aws::Vector<unsigned char>& dest, const unsigned char** src_start, size_t count, size_t& remain)
	{
		dest.insert(dest.end(), *src_start, (*src_start) + count);
		(*src_start) += count;
		remain -= count;
	};

	copy_and_update(buffer, &ptr_cast_pos, copy_count, remain);

	// upload the content of the buffer until the size of the remaining data is smaller than the minimum upload size
	while (buffer.size() >= WriteFile::buff_min_)
	{
		auto outcome = UploadPart(*h_ptr);
		RETURN_ON_ERROR(outcome, "Error during upload", kBadSize);

		// copy remaining data up to capacity
		buffer.clear();
		copy_count = std::min(remain, buffer.capacity());
		copy_and_update(buffer, &ptr_cast_pos, copy_count, remain);
	}

	// release unused memory
	buffer.shrink_to_fit();

	return static_cast<long long>(to_write);
}

int driver_fflush(void*)
{
	KH_S3_NOT_CONNECTED(kBadSize);

	spdlog::debug("Flushing (does nothing...)");
	return 0;
}

int driver_remove(const char* filename)
{
	KH_S3_NOT_CONNECTED(kFalse);

	ERROR_ON_NULL_ARG(filename, kFalse);

	spdlog::debug("remove {}", filename);

	NAMES_OR_ERROR(filename, kFalse);

	// auto maybe_parsed_names = ParseS3Uri(filename);
	// ERROR_ON_NAMES(maybe_parsed_names, kFalse);
	// auto& names = maybe_parsed_names.GetResult();
	// std::string bucket_name, object_name;
	// ParseS3Uri(filename, bucket_name, object_name);
	// FallbackToDefaultBucket(bucket_name);

	Aws::S3::Model::DeleteObjectRequest request;

	request.WithBucket(names.bucket_).WithKey(names.object_);

	Aws::S3::Model::DeleteObjectOutcome outcome = client->DeleteObject(request);

	if (!outcome.IsSuccess())
	{
		auto err = outcome.GetError();
		spdlog::error("DeleteObject: {} {}", err.GetExceptionName(), err.GetMessage());
	}

	return outcome.IsSuccess();
}

int driver_rmdir(const char* filename)
{
	KH_S3_NOT_CONNECTED(kFailure);

	ERROR_ON_NULL_ARG(filename, kFailure);
	spdlog::debug("rmdir {}", filename);

	spdlog::debug("Remove dir (does nothing...)");
	return kSuccess;
}

int driver_mkdir(const char* filename)
{
	KH_S3_NOT_CONNECTED(kFailure);

	ERROR_ON_NULL_ARG(filename, kFailure);
	spdlog::debug("mkdir {}", filename);

	return 1;
}

long long int driver_diskFreeSpace(const char* filename)
{
	spdlog::debug("diskFreeSpace {}", filename);

	return (long long int)5 * 1024 * 1024 * 1024 * 1024;
}

int driver_copyToLocal(const char* sSourceFilePathName, const char* sDestFilePathName)
{
	KH_S3_NOT_CONNECTED(kFailure);
	ERROR_ON_NULL_ARG(sSourceFilePathName, kFailure);
	ERROR_ON_NULL_ARG(sDestFilePathName, kFailure);

	spdlog::debug("copyToLocal {} {}", sSourceFilePathName, sDestFilePathName);

	// try opening the online source file
	NAMES_OR_ERROR(sSourceFilePathName, kFailure);
	auto make_reader_outcome = MakeReaderPtr(names.bucket_, names.object_);
	RETURN_ON_ERROR(make_reader_outcome, "Error while opening remote file", kFailure);

	// open local file
	std::ofstream file_stream(sDestFilePathName, std::ios::binary);
	if (!file_stream.is_open())
	{
		std::ostringstream oss;
		oss << "Failed to open local file for writing: " << sDestFilePathName;
		LogError(oss.str());
		return kFailure;
	}

	auto read_and_write = [](const Reader& from, size_t part, std::ofstream& to_file) -> bool
	{
		// file metadata
		const long long header_size = from.common_header_length_;

		// limit download to a few MBs at a time.
		constexpr long long dl_limit{10 * 1024 * 1024};

		const long long file_size = part == 0 ? 
			from.cumulative_sizes_[0] : header_size + from.cumulative_sizes_[part]-from.cumulative_sizes_[part-1];

		// download range limits
		const long long end_limit = file_size - 1;
		long long start = 0 == part ? 0 : header_size;
		long long end = std::min(start + dl_limit - 1, end_limit);

		//download by pieces
		while (to_file && start < end_limit)
		{
			const auto request =
			    MakeGetObjectRequest(from.bucketname_, from.filenames_[part], MakeByteRange(start, end));
			auto get_outcome = client->GetObject(request);
			RETURN_ON_ERROR(get_outcome, "Error while downloading file content", false);

			// get ownership of the result and its underlying stream
			const Aws::S3::Model::GetObjectResult result{get_outcome.GetResultWithOwnership()};
			to_file << result.GetBody().rdbuf();

			start +=
			    result
				.GetContentLength(); // a bit of security for now: could the downloading be incomplete?
			end = std::min(start + dl_limit - 1, end_limit);
		}
		// what made the process stop?
		if (!to_file)
		{
			// something went wrong on write side, abort
			LogError("Error while writing data to local file");
			return false;
		}

		return true;
	};

	const Reader& reader = *(make_reader_outcome.GetResult());
	const size_t parts_count{reader.filenames_.size()};

	bool op_res = true;
	for (size_t part = 0; part < parts_count && op_res; part++)
	{
		op_res = read_and_write(reader, part, file_stream);
	}

	file_stream.close();

	if (!op_res || !file_stream)
	{
		LogError("Error copying remote file to local storage.");
		spdlog::debug("Attempting to remove local file.");
		if (0 != std::remove(sDestFilePathName))
		{
			LogError("Error attempting to remove local file.");
		}
		spdlog::debug("Successful file removal.");

		return kFailure;
	}

	spdlog::debug("Successful local copy of remote file.");

	return kSuccess;
}

int driver_copyFromLocal(const char* sSourceFilePathName, const char* sDestFilePathName)
{
	KH_S3_NOT_CONNECTED(kBadSize);

	ERROR_ON_NULL_ARG(sSourceFilePathName, kFailure);
	ERROR_ON_NULL_ARG(sDestFilePathName, kFailure);

	spdlog::debug("copyFromLocal {} {}", sSourceFilePathName, sDestFilePathName);

	NAMES_OR_ERROR(sDestFilePathName, kFailure);
	// std::string bucket_name, object_name;
	// ParseS3Uri(sDestFilePathName, bucket_name, object_name);
	// FallbackToDefaultBucket(bucket_name);

	// Configuration de la requête pour envoyer l'objet
	Aws::S3::Model::PutObjectRequest object_request;
	object_request.WithBucket(names.bucket_).WithKey(names.object_);

	// Chargement du fichier dans un flux d'entrée
	std::shared_ptr<Aws::IOStream> input_data = Aws::MakeShared<Aws::FStream>(
	    "PutObjectInputStream", sSourceFilePathName, std::ios_base::in | std::ios_base::binary);

	object_request.SetBody(input_data);

	// Exécution de la requête
	auto put_object_outcome = client->PutObject(object_request);

	if (!put_object_outcome.IsSuccess())
	{
		spdlog::error("Error during file upload: {}", put_object_outcome.GetError().GetMessage());
		return false;
	}

	return true;
}

bool test_compareFiles(const char* local_file_path_str, const char* s3_uri_str) {
  std::string local_file_path(local_file_path_str);
  std::string s3_uri(s3_uri_str);

  // Lire le fichier local
  std::ifstream local_file(local_file_path, std::ios::binary);
  if (!local_file) {
    std::cerr << "Failure reading local file" << std::endl;
    return false;
  }
  std::string local_content((std::istreambuf_iterator<char>(local_file)),
                            std::istreambuf_iterator<char>());

  // Télécharger l'objet S3
  char const *prefix = "s3://";
  const size_t prefix_size{std::strlen(prefix)};
  const size_t pos = s3_uri.find('/', prefix_size);
  std::string bucket_name = s3_uri.substr(prefix_size, pos - prefix_size);
  std::string object_name = s3_uri.substr(pos + 1);

  // Télécharger l'objet S3
  Aws::S3::Model::GetObjectRequest object_request;
  object_request.SetBucket(bucket_name.c_str());
  object_request.SetKey(object_name.c_str());
  auto get_object_outcome = client->GetObject(object_request);
  if (!get_object_outcome.IsSuccess()) {
    std::cerr << "Failure retrieving object from S3" << std::endl;
    return false;
  }

  // Lire le contenu de l'objet S3
  std::stringstream s3_content;
  s3_content << get_object_outcome.GetResult().GetBody().rdbuf();

  // Comparer les contenus
  auto result = local_content == s3_content.str() ? kSuccess : kFailure;

  return static_cast<bool>(result);
}
