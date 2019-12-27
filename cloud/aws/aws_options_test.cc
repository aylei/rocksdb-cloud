#include <cctype>
#include <cinttypes>
#include <cstring>
#include <unordered_map>

#include "cloud/aws/aws_env.h"
#include "rocksdb/cloud/cloud_log_controller.h"
#include "rocksdb/cloud/cloud_storage_provider.h"
#include "rocksdb/utilities/object_registry.h"
#include "test_util/testharness.h"
#include "test_util/testutil.h"

#ifndef GFLAGS
bool FLAGS_enable_print = false;
#else
#include "util/gflags_compat.h"
using GFLAGS_NAMESPACE::ParseCommandLineFlags;
DEFINE_bool(enable_print, false, "Print options generated to console.");
#endif  // GFLAGS

namespace rocksdb {

class AwsOptionsTest : public testing::Test {};

#ifndef ROCKSDB_LITE
static Status RegisterAws(std::shared_ptr<ObjectRegistry> & registry) {
  Status s;
#ifdef ROCKSDB_DLL
  std::shared_ptr<DynamicLibrary> library;
  s = Env::Default()->LoadLibrary("rocksdb_cloud_debug", "", &library);
  if (s.ok()) {
    s = registry->AddDynamicLibrary(library, "RegisterAwsObjects", "AWS");
  }
#else
  std::shared_ptr<ObjectLibrary> library = registry->AddLocalLibrary("aws");
  library->Register(RegisterAwsObjects, "AWS");
#endif // ROCKSDB_DLL
  return s;
}
#endif // ROCKSDB_LITE

#ifndef ROCKSDB_LITE
#ifdef USE_AWS
TEST_F(AwsOptionsTest, TestLoadAws) {
  DBOptions db_opts;
  ConfigOptions cfg;
  ASSERT_OK(RegisterAws(cfg.registry));
  ASSERT_OK(Env::CreateFromString(
         "bucket.source={bucket=test;object=path;region=east}; "
         "bucket.dest={bucket=test;object=path;region=west}; "
         "id=AWS; storage_provider=s3; log_controller=kinesis", cfg,
         &db_opts.env));
  ASSERT_NE(db_opts.env, nullptr);
  ASSERT_NOK(db_opts.env->SanitizeOptions(db_opts)); // AWS requires regions to match
  delete db_opts.env; db_opts.env = nullptr;

  ASSERT_OK(Env::CreateFromString(
         "bucket.source={bucket=test;object=path}; "
         "bucket.dest={bucket=test;object=path}; "
         "id=AWS; storage_provider=s3;", cfg,
         &db_opts.env));
  ASSERT_NE(db_opts.env, nullptr);
  auto *options = db_opts.env->GetOptions<CloudEnvOptions>("CloudEnvOptions");
  ASSERT_NE(options, nullptr);
  ASSERT_NE(options->storage_provider, nullptr);
  ASSERT_EQ(options->storage_provider->GetId(), "s3");
  ASSERT_OK(db_opts.env->SanitizeOptions(db_opts));

  CloudEnv *cloud = db_opts.env->AsEnv<CloudEnv>("AWS");
  ASSERT_NE(cloud, nullptr);
  ASSERT_EQ(cloud, db_opts.env);
  cloud = db_opts.env->AsEnv<CloudEnv>("CloudEnvImpl");
  ASSERT_NE(cloud, nullptr);
  ASSERT_EQ(cloud, db_opts.env);
  cloud = db_opts.env->AsEnv<CloudEnv>("CloudEnv");
  ASSERT_NE(cloud, nullptr);
  ASSERT_EQ(cloud, db_opts.env);
  delete db_opts.env; db_opts.env = nullptr;
}
  
TEST_F(AwsOptionsTest, TestAwsEnvOptions) {
  Env* aws = nullptr;
  Env* copy = nullptr;

  ConfigOptions cfg;
  ASSERT_OK(RegisterAws(cfg.registry));
  ASSERT_OK(Env::CreateFromString("id=AWS", cfg, &aws));
  ASSERT_OK(Env::CreateFromString("id=AWS", cfg, &copy));

  std::string opt_str;
  ASSERT_OK(aws->ConfigureFromString(
         "aws.server_side_encryption=true; aws.encryption_key_id=my-key; aws.use_transfer_manager=false", cfg));
  const auto *options = aws->GetOptions<CloudEnvOptions>("CloudEnvOptions");
  ASSERT_NE(options, nullptr);
  ASSERT_TRUE(options->server_side_encryption);
  ASSERT_EQ(options->encryption_key_id, "my-key");
  ASSERT_FALSE(options->use_aws_transfer_manager);
  ASSERT_OK(aws->GetOptionString(cfg, &opt_str));
  ASSERT_OK(copy->ConfigureFromString(opt_str, cfg));
  ASSERT_TRUE(copy->Matches(aws, cfg));
              
  ASSERT_OK(aws->ConfigureFromString(
         "aws.server_side_encryption=false; aws.encryption_key_id=; aws.use_transfer_manager=true", cfg));
  ASSERT_FALSE(options->server_side_encryption);
  ASSERT_EQ(options->encryption_key_id, "");
  ASSERT_TRUE(options->use_aws_transfer_manager);
  delete aws; 
  delete copy;
}
  
TEST_F(AwsOptionsTest, TestAwsCredentialOptions) {
  // Note that this test does not compile when loading against a shared library because
  // the symbols are defined in the shared library that is not linked into the executable
#ifndef ROCKSDB_DLL
  Env* aws = nullptr;
  ConfigOptions cfg;
  ASSERT_OK(RegisterAws(cfg.registry));
  ASSERT_OK(Env::CreateFromString("id=AWS", cfg, &aws));

  auto *creds = aws->GetOptions<AwsCloudAccessCredentials>("AwsCredentials");
  bool has_env_creds = (getenv("AWS_ACCESS_KEY_ID") != nullptr &&
                        getenv("AWS_SECRET_ACCESS_KEY") != nullptr);
  ASSERT_NE(creds, nullptr);
  if (has_env_creds) {
    ASSERT_EQ(creds->GetAccessType(), AwsAccessType::kEnvironment);
    ASSERT_OK(creds->HasValid());
  } else {
    ASSERT_EQ(creds->GetAccessType(), AwsAccessType::kUndefined);
    ASSERT_NOK(creds->HasValid());
  }

  // Test simple creds.  Simple are valid if both keys are specified or in the environment
  ASSERT_OK(aws->ConfigureFromString(
         "aws.credentials.type=simple", cfg));
  ASSERT_EQ(creds->GetAccessType(), AwsAccessType::kSimple);
  ASSERT_EQ(creds->HasValid().ok(), has_env_creds);
  // Set one
  ASSERT_OK(aws->ConfigureFromString(
         "aws.credentials.type=undefined; aws.credentials.access_key_id=access", cfg));
  ASSERT_EQ(creds->GetAccessType(), AwsAccessType::kSimple);
  ASSERT_EQ(creds->HasValid().ok(), getenv("AWS_SECRET_ACCESS_KEY") != nullptr);
  
  // Set both
  ASSERT_OK(aws->ConfigureFromString(
         "aws.credentials.type=undefined; aws.credentials.secret_key=secret", cfg));
  ASSERT_EQ(creds->GetAccessType(), AwsAccessType::kSimple);
  ASSERT_OK(creds->HasValid());
  // Set just the other
  ASSERT_OK(aws->ConfigureFromString(
         "aws.credentials.type=undefined; aws.credentials.access_key_id=", cfg));
  ASSERT_EQ(creds->GetAccessType(), AwsAccessType::kSimple);
  ASSERT_EQ(creds->HasValid().ok(), getenv("AWS_ACCESS_KEY_ID") != nullptr);

  // Test config credentials
  ASSERT_OK(aws->ConfigureFromString(
         "aws.credentials.config_file=file; aws.credentials.secret_key=; aws.credentials.access_key_id=", cfg));
  ASSERT_OK(creds->HasValid());
  ASSERT_EQ(creds->GetAccessType(), AwsAccessType::kConfig);
  
  ASSERT_OK(aws->ConfigureFromString(
         "aws.credentials.type=config; aws.credentials.config_file=", cfg));
  ASSERT_OK(creds->HasValid());
  ASSERT_EQ(creds->GetAccessType(), AwsAccessType::kConfig);

  ASSERT_OK(aws->ConfigureFromString(
         "aws.credentials.type=undefined; aws.credentials.config_file=", cfg));
  if (has_env_creds) {
    ASSERT_OK(creds->HasValid());
    ASSERT_EQ(creds->GetAccessType(), AwsAccessType::kEnvironment);
  } else {
    ASSERT_NOK(creds->HasValid());
    ASSERT_EQ(creds->GetAccessType(), AwsAccessType::kUndefined);
  }
  
  // Now test the instance/anonymous
  ASSERT_OK(aws->ConfigureFromString("aws.credentials.type=anonymous", cfg));
  ASSERT_OK(creds->HasValid());
  ASSERT_EQ(creds->GetAccessType(), AwsAccessType::kAnonymous);
  
  ASSERT_OK(aws->ConfigureFromString("aws.credentials.type=instance", cfg));
  ASSERT_OK(creds->HasValid());
  ASSERT_EQ(creds->GetAccessType(), AwsAccessType::kInstance);
  
  ASSERT_OK(aws->ConfigureFromString("aws.credentials.type=EC2", cfg));
  ASSERT_OK(creds->HasValid());
  ASSERT_EQ(creds->GetAccessType(), AwsAccessType::kInstance);
  delete aws;
#endif // ROCKSDB_DLL
}
#endif // USE_AWS
  
#endif  // !ROCKSDB_LITE
}  // namespace rocksdb

#include <stdio.h>
#include <execinfo.h>
#include <signal.h>
#include <stdlib.h>
#include <unistd.h>

void handler(int sig) {
  void *array[10];
  size_t size;

  // get void*'s for all entries on the stack
  size = backtrace(array, 10);

  // print out all the frames to stderr
  fprintf(stderr, "Error: signal %d:\n", sig);
  backtrace_symbols_fd(array, (int) size, STDERR_FILENO);
  exit(1);
}
int main(int argc, char** argv) {
  signal(SIGSEGV, handler);   // install our handler
  ::testing::InitGoogleTest(&argc, argv);
#ifdef GFLAGS
  ParseCommandLineFlags(&argc, &argv, true);
#endif  // GFLAGS
  return RUN_ALL_TESTS();
}