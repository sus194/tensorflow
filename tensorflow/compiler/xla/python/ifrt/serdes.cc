/* Copyright 2023 The TensorFlow Authors. All Rights Reserved.

Licensed under the Apache License, Version 2.0 (the "License");
you may not use this file except in compliance with the License.
You may obtain a copy of the License at

    http://www.apache.org/licenses/LICENSE-2.0

Unless required by applicable law or agreed to in writing, software
distributed under the License is distributed on an "AS IS" BASIS,
WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
See the License for the specific language governing permissions and
limitations under the License.
==============================================================================*/

#include "tensorflow/compiler/xla/python/ifrt/serdes.h"

#include <memory>
#include <string>
#include <utility>

#include "absl/base/thread_annotations.h"
#include "absl/container/flat_hash_map.h"
#include "absl/log/check.h"
#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include "absl/strings/string_view.h"
#include "absl/synchronization/mutex.h"
#include "tensorflow/compiler/xla/python/ifrt/serdes.pb.h"
#include "tensorflow/tsl/platform/statusor.h"

namespace xla {
namespace ifrt {

namespace {

struct Registry {
  absl::Mutex mu;

  // Mapping from LLVM RTTI type ids of `Serializable` to `SerDes`. Used during
  // serialization, which is aware of the LLVM RTTI type id.
  absl::flat_hash_map<const void*, SerDes*> type_id_to_serdes
      ABSL_GUARDED_BY(mu);

  // Mapping from `SerDes::Name()` to `SerDes`. Used during deserialization,
  // which uses the type name encoded in the serialized string.
  absl::flat_hash_map<absl::string_view, SerDes*> name_to_serdes
      ABSL_GUARDED_BY(mu);
};

Registry* registry() {
  static auto* r = new Registry();
  return r;
}

}  // namespace

char Serializable::ID = 0;
char SerDes::ID = 0;

void RegisterSerDes(const void* type_id, std::unique_ptr<SerDes> serdes) {
  Registry* const r = registry();
  absl::MutexLock l(&r->mu);

  CHECK(r->type_id_to_serdes.insert({type_id, serdes.get()}).second)
      << "xla::ifrt::SerDes cannot be registered more than once for the same "
         "type id: "
      << type_id;

  const absl::string_view name = serdes->type_name();
  CHECK(r->name_to_serdes.insert({name, serdes.get()}).second)
      << "xla::ifrt::SerDes cannot be registered more than once for the same "
         "name: "
      << name;

  // `SerDes` must be kept alive until the process exit. Since global variables
  // should not have destructors, we can just release the unique ptr.
  serdes.release();
}

absl::StatusOr<Serialized> Serialize(const Serializable& serializable) {
  SerDes* serdes;
  {
    Registry* const r = registry();
    absl::MutexLock l(&r->mu);
    auto it = r->type_id_to_serdes.find(serializable.dynamicClassID());
    if (it == r->type_id_to_serdes.end()) {
      return absl::UnimplementedError(
          "Serializable has no associated SerDes implementation");
    }
    serdes = it->second;
  }
  TF_ASSIGN_OR_RETURN(std::string data, serdes->Serialize(serializable));

  Serialized proto;
  proto.set_type_name(std::string(serdes->type_name()));
  proto.set_data(std::move(data));
  return proto;
}

absl::StatusOr<std::unique_ptr<Serializable>> Deserialize(
    const Serialized& serialized) {
  SerDes* serdes;
  {
    Registry* const r = registry();
    absl::MutexLock l(&r->mu);
    auto it = r->name_to_serdes.find(serialized.type_name());
    if (it == r->name_to_serdes.end()) {
      return absl::UnimplementedError(
          "Serializable has no associated SerDes implementation");
    }
    serdes = it->second;
  }
  return serdes->Deserialize(serialized.data());
}

}  // namespace ifrt
}  // namespace xla
