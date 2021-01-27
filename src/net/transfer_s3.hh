#pragma once

#include <atomic>

#include "net/s3.hh"
#include "net/secure_socket.hh"
#include "net/socket.hh"
#include "storage/backend_s3.hh"
#include "transfer.hh"

constexpr std::chrono::seconds ADDR_UPDATE_INTERVAL { 25 };

class S3TransferAgent : public TransferAgent
{
protected:
  struct S3Config
  {
    AWSCredentials credentials {};
    std::string region {};
    std::string bucket {};
    std::string prefix {};

    std::string endpoint {};
    std::atomic<Address> address { Address { "0", 0 } };

    S3Config( const S3StorageBackend& backend );
  } _client_config;

  static constexpr size_t MAX_REQUESTS_ON_CONNECTION { 1 };
  std::chrono::steady_clock::time_point _last_addr_update {};
  const bool _upload_as_public;

  HTTPRequest get_request( const Action& action );

  void do_action( Action&& action ) override;
  void worker_thread( const size_t thread_id ) override;

  std::atomic<uint64_t>* _byte_counter { nullptr };

public:
  S3TransferAgent( const S3StorageBackend& backend,
                   const size_t thread_count = MAX_THREADS,
                   const bool upload_as_public = false,
                   std::atomic<uint64_t>* byte_counter = nullptr );

  S3TransferAgent( const S3TransferAgent& ) = delete;
  S3TransferAgent& operator=( const S3TransferAgent& ) = delete;

  ~S3TransferAgent();
};
