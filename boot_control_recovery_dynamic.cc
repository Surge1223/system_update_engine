//
// Copyright (C) 2015 The Android Open Source Project
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//      http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.
//

#include "update_engine/boot_control_recovery.h"

#include <memory>
#include <utility>
#include <vector>

#include <base/bind.h>
#include <base/files/file_util.h>
#include <base/logging.h>
#include <base/strings/string_util.h>
#include <bootloader_message/bootloader_message.h>
#include <brillo/message_loops/message_loop.h>
#include <fs_mgr.h>
#include <fs_mgr_overlayfs.h>

#include "update_engine/common/utils.h"
#include "update_engine/dynamic_partition_control_android.h"
#include "update_engine/utils_android.h"

using std::string;

using android::dm::DmDeviceState;
using android::fs_mgr::Partition;
using android::hardware::hidl_string;
using android::hardware::Return;
using android::hardware::boot::V1_0::BoolResult;
using android::hardware::boot::V1_0::CommandResult;
using android::hardware::boot::V1_0::IBootControl;
using Slot = chromeos_update_engine::BootControlInterface::Slot;
using PartitionMetadata =
    chromeos_update_engine::BootControlInterface::PartitionMetadata;

// When called from update_engine_sideload, we don't attempt to dynamically load
// the right boot_control HAL, instead we use the only HAL statically linked in
// via the PRODUCT_STATIC_BOOT_CONTROL_HAL make variable and access the module
// struct directly.
extern const hw_module_t HAL_MODULE_INFO_SYM;

namespace chromeos_update_engine {

namespace boot_control {

namespace {

auto StoreResultCallback(CommandResult* dest) {
  return [dest](const CommandResult& result) { *dest = result; };
}
}  // namespace

// Factory defined in boot_control.h.
std::unique_ptr<BootControlInterface> CreateBootControl() {
  std::unique_ptr<BootControlRecovery> boot_control(new BootControlRecovery());
  auto boot_control2 = std::make_unique<BootControlRecovery>();
  bool tryboth = false;
  if (!boot_control->Init()) {
  LOG(ERROR) << "Attempting to native HAL ";
  tryboth = true;
  return nullptr;
}
 if (!tryboth) {
  return std::move(boot_control);
  } else if (!boot_control2->Init()) {
    return nullptr;
  }
  return std::move(boot_control2);
}

}  // namespace boot_control
bool BootControlRecovery::Init() {
  moduleN_ = IBootControl::getService();
  const hw_module_t* hw_module;
  int ret;
  // For update_engine_sideload, we simulate the hw_get_module() by accessing it
  // from the current process directly.
  hw_module = &HAL_MODULE_INFO_SYM;
  ret = 0;
  if (!hw_module ||
      strcmp(BOOT_CONTROL_HARDWARE_MODULE_ID, hw_module->id) != 0) {
    ret = -EINVAL;
  }
  if (ret != 0) {
    LOG(ERROR) << "Error loading all boot_control HAL implementations.";
   // return false;
  }
  if (moduleN_ == nullptr) {
    LOG(ERROR) << "Error getting bootctrl HIDL module ";
    LOG(ERROR) << "Attempting to use the hardware HAL ";
      return false;
 }
  module_ = reinterpret_cast<boot_control_module_t*>(
      const_cast<hw_module_t*>(hw_module));
  module_->init(module_);

  LOG(INFO) << "Loaded boot_control HAL "
            << "'" << hw_module->name << "' "
            << "version " << (hw_module->module_api_version >> 8) << "."
            << (hw_module->module_api_version & 0xff) << " "
            << "authored by '" << hw_module->author << "'.";

  dynamic_control_ = std::make_unique<DynamicPartitionControlAndroid>();
  LOG(INFO) << "Loaded boot control hidl hal.";
    return true;
}

void BootControlRecovery::Cleanup() {
  dynamic_control_->Cleanup();
}


unsigned int BootControlRecovery::GetNumSlots() const {
  return module_->getNumberSlots(module_);
}

BootControlInterface::Slot BootControlRecovery::GetCurrentSlot() const {
  return module_->getCurrentSlot(module_);
}

bool BootControlRecovery::GetSuffix(Slot slot, string* suffix) const {
  auto store_suffix_cb = [&suffix](hidl_string cb_suffix) {
    *suffix = cb_suffix.c_str();
  };
  Return<void> ret = moduleN_->getSuffix(slot, store_suffix_cb);

  if (!ret.isOk()) {
    LOG(ERROR) << "boot_control impl returned no suffix for slot "
               << SlotName(slot);
    return false;
  }
  return true;
}

bool BootControlRecovery::IsSuperBlockDevice(
    const base::FilePath& device_dir,
    Slot slot,
    const string& partition_name_suffix) const {
  string source_device =
      device_dir.Append(fs_mgr_get_super_partition_name(slot)).value();
  auto source_metadata = dynamic_control_->LoadMetadataBuilder(
      source_device, slot, BootControlInterface::kInvalidSlot);
  return source_metadata->HasBlockDevice(partition_name_suffix);
}

BootControlRecovery::DynamicPartitionDeviceStatus
BootControlRecovery::GetDynamicPartitionDevice(
    const base::FilePath& device_dir,
    const string& partition_name_suffix,
    Slot slot,
    string* device) const {
  string super_device =
      device_dir.Append(fs_mgr_get_super_partition_name(slot)).value();

  auto builder = dynamic_control_->LoadMetadataBuilder(
      super_device, slot, BootControlInterface::kInvalidSlot);

  if (builder == nullptr) {
    LOG(ERROR) << "No metadata in slot "
               << BootControlInterface::SlotName(slot);
    return DynamicPartitionDeviceStatus::ERROR;
  }

  Slot current_slot = GetCurrentSlot();
  if (builder->FindPartition(partition_name_suffix) == nullptr) {
    LOG(INFO) << partition_name_suffix
              << " is not in super partition metadata.";

    if (IsSuperBlockDevice(device_dir, current_slot, partition_name_suffix)) {
      LOG(ERROR) << "The static partition " << partition_name_suffix
                 << " is a block device for current metadata ("
                 << fs_mgr_get_super_partition_name(current_slot) << ", slot "
                 << BootControlInterface::SlotName(current_slot)
                 << "). It cannot be used as a logical partition.";
      return DynamicPartitionDeviceStatus::ERROR;
    }

    return DynamicPartitionDeviceStatus::TRY_STATIC;
  }

  if (slot == current_slot) {
    if (dynamic_control_->GetState(partition_name_suffix) !=
        DmDeviceState::ACTIVE) {
      LOG(WARNING) << partition_name_suffix << " is at current slot but it is "
                   << "not mapped. Now try to map it.";
    } else {
      if (dynamic_control_->GetDmDevicePathByName(partition_name_suffix,
                                                  device)) {
        LOG(INFO) << partition_name_suffix
                  << " is mapped on device mapper: " << *device;
        return DynamicPartitionDeviceStatus::SUCCESS;
      }
      LOG(ERROR) << partition_name_suffix << "is mapped but path is unknown.";
      return DynamicPartitionDeviceStatus::ERROR;
    }
  }

  bool force_writable = slot != current_slot;
  if (dynamic_control_->MapPartitionOnDeviceMapper(
          super_device, partition_name_suffix, slot, force_writable, device)) {
    return DynamicPartitionDeviceStatus::SUCCESS;
  }
  return DynamicPartitionDeviceStatus::ERROR;
}

bool BootControlRecovery::GetPartitionDevice(const string& partition_name,
                                            Slot slot,
                                            string* device) const {

  // We can't use fs_mgr to look up |partition_name| because fstab
  // doesn't list every slot partition (it uses the slotselect option
  // to mask the suffix).
  //
  // We can however assume that there's an entry for the /misc mount
  // point and use that to get the device file for the misc
  // partition. This helps us locate the disk that |partition_name|
  // resides on. From there we'll assume that a by-name scheme is used
  // so we can just replace the trailing "misc" by the given
  // |partition_name| and suffix corresponding to |slot|, e.g.
  //
  //   /dev/block/platform/soc.0/7824900.sdhci/by-name/misc ->
  //   /dev/block/platform/soc.0/7824900.sdhci/by-name/boot_a
  //
  // If needed, it's possible to relax the by-name assumption in the
  // future by trawling /sys/block looking for the appropriate sibling
  // of misc and then finding an entry in /dev matching the sysfs
  // entry.

  string suffix;
  if (!GetSuffix(slot, &suffix)) {
    return false;
  }
  const string partition_name_suffix = partition_name + suffix;

  string device_dir_str;
  if (!dynamic_control_->GetDeviceDir(&device_dir_str)) {
    return false;
  }
  base::FilePath device_dir(device_dir_str);

  // When looking up target partition devices, treat them as static if the
  // current payload doesn't encode them as dynamic partitions. This may happen
  // when applying a retrofit update on top of a dynamic-partitions-enabled
  // build.
  if (dynamic_control_->IsDynamicPartitionsEnabled() &&
      (slot == GetCurrentSlot() || is_target_dynamic_)) {
    switch (GetDynamicPartitionDevice(
        device_dir, partition_name_suffix, slot, device)) {
      case DynamicPartitionDeviceStatus::SUCCESS:
        return true;
      case DynamicPartitionDeviceStatus::TRY_STATIC:
        break;
      case DynamicPartitionDeviceStatus::ERROR:  // fallthrough
      default:
        return false;
    }
  }

  base::FilePath path = device_dir.Append(partition_name_suffix);
  if (!dynamic_control_->DeviceExists(path.value())) {
    LOG(ERROR) << "Device file " << path.value() << " does not exist.";
    return false;
  }

  *device = path.value();
  return true;
}

bool BootControlRecovery::IsSlotBootable(Slot slot) const {
  Return<BoolResult> ret = moduleN_->isSlotBootable(slot);
  if (!ret.isOk()) {
    LOG(ERROR) << "Unable to determine if slot " << SlotName(slot)
               << " is bootable: " << ret.description();
    return false;
  }
  if (ret == BoolResult::INVALID_SLOT) {
    LOG(ERROR) << "Invalid slot: " << SlotName(slot);
    return false;
  }
  return ret == BoolResult::TRUE;
}

bool BootControlRecovery::MarkSlotUnbootable(Slot slot) {
  CommandResult result;
  auto ret = moduleN_->setSlotAsUnbootable(slot, boot_control::StoreResultCallback(&result));
  if (!ret.isOk()) {
    LOG(ERROR) << "Unable to call MarkSlotUnbootable for slot "
               << SlotName(slot) << ": " << ret.description();
    return false;
  }
  if (!result.success) {
    LOG(ERROR) << "Unable to mark slot " << SlotName(slot)
               << " as unbootable: " << result.errMsg.c_str();
  }
  return result.success;
}

bool BootControlRecovery::SetActiveBootSlot(Slot slot) {
  CommandResult result;
  auto ret = moduleN_->setActiveBootSlot(slot, boot_control::StoreResultCallback(&result));
  if (!ret.isOk()) {
    LOG(ERROR) << "Unable to call SetActiveBootSlot for slot " << SlotName(slot)
               << ": " << ret.description();
    return false;
  }
  if (!result.success) {
    LOG(ERROR) << "Unable to set the active slot to slot " << SlotName(slot)
               << ": " << result.errMsg.c_str();
  }
  return result.success;
}

bool BootControlRecovery::MarkBootSuccessfulAsync(
    base::Callback<void(bool)> callback) {
  CommandResult result;
  auto ret = moduleN_->markBootSuccessful(boot_control::StoreResultCallback(&result));
  if (!ret.isOk()) {
    LOG(ERROR) << "Unable to call MarkBootSuccessful: " << ret.description();
    return false;
  }
  if (!result.success) {
    LOG(ERROR) << "Unable to mark boot successful: " << result.errMsg.c_str();
  }
  return brillo::MessageLoop::current()->PostTask(
             FROM_HERE, base::Bind(callback, result.success)) !=
         brillo::MessageLoop::kTaskIdNull;
}

namespace {

bool UpdatePartitionMetadata(DynamicPartitionControlInterface* dynamic_control,
                             Slot source_slot,
                             Slot target_slot,
                             const string& target_suffix,
                             const PartitionMetadata& partition_metadata) {
  string device_dir_str;
  if (!dynamic_control->GetDeviceDir(&device_dir_str)) {
    return false;
  }
  base::FilePath device_dir(device_dir_str);
  auto source_device =
      device_dir.Append(fs_mgr_get_super_partition_name(source_slot)).value();

  auto builder = dynamic_control->LoadMetadataBuilder(
      source_device, source_slot, target_slot);
  if (builder == nullptr) {
    // TODO(elsk): allow reconstructing metadata from partition_metadata
    // in recovery sideload.
    LOG(ERROR) << "No metadata at "
               << BootControlInterface::SlotName(source_slot);
    return false;
  }

  std::vector<string> groups = builder->ListGroups();
  for (const auto& group_name : groups) {
    if (base::EndsWith(
            group_name, target_suffix, base::CompareCase::SENSITIVE)) {
      LOG(INFO) << "Removing group " << group_name;
      builder->RemoveGroupAndPartitions(group_name);
    }
  }

  uint64_t total_size = 0;
  for (const auto& group : partition_metadata.groups) {
    total_size += group.size;
  }

  string expr;
  uint64_t allocatable_space = builder->AllocatableSpace();
  if (!dynamic_control->IsDynamicPartitionsRetrofit()) {
    allocatable_space /= 2;
    expr = "half of ";
  }
  if (total_size > allocatable_space) {
    LOG(ERROR) << "The maximum size of all groups with suffix " << target_suffix
               << " (" << total_size << ") has exceeded " << expr
               << " allocatable space for dynamic partitions "
               << allocatable_space << ".";
    return false;
  }

  for (const auto& group : partition_metadata.groups) {
    auto group_name_suffix = group.name + target_suffix;
    if (!builder->AddGroup(group_name_suffix, group.size)) {
      LOG(ERROR) << "Cannot add group " << group_name_suffix << " with size "
                 << group.size;
      return false;
    }
    LOG(INFO) << "Added group " << group_name_suffix << " with size "
              << group.size;

    for (const auto& partition : group.partitions) {
      auto partition_name_suffix = partition.name + target_suffix;
      Partition* p = builder->AddPartition(
          partition_name_suffix, group_name_suffix, LP_PARTITION_ATTR_READONLY);
      if (!p) {
        LOG(ERROR) << "Cannot add partition " << partition_name_suffix
                   << " to group " << group_name_suffix;
        return false;
      }
      if (!builder->ResizePartition(p, partition.size)) {
        LOG(ERROR) << "Cannot resize partition " << partition_name_suffix
                   << " to size " << partition.size << ". Not enough space?";
        return false;
      }
      LOG(INFO) << "Added partition " << partition_name_suffix << " to group "
                << group_name_suffix << " with size " << partition.size;
    }
  }

  auto target_device =
      device_dir.Append(fs_mgr_get_super_partition_name(target_slot)).value();
  return dynamic_control->StoreMetadata(
      target_device, builder.get(), target_slot);
}

bool UnmapTargetPartitions(DynamicPartitionControlInterface* dynamic_control,
                           const string& target_suffix,
                           const PartitionMetadata& partition_metadata) {
  for (const auto& group : partition_metadata.groups) {
    for (const auto& partition : group.partitions) {
      if (!dynamic_control->UnmapPartitionOnDeviceMapper(
              partition.name + target_suffix, true /* wait */)) {
        return false;
      }
    }
  }
  return true;
}

}  // namespace

bool BootControlRecovery::InitPartitionMetadata(
    Slot target_slot,
    const PartitionMetadata& partition_metadata,
    bool update_metadata) {
  if (fs_mgr_overlayfs_is_setup()) {
    // Non DAP devices can use overlayfs as well.
    LOG(WARNING)
        << "overlayfs overrides are active and can interfere with our "
           "resources.\n"
        << "run adb enable-verity to deactivate if required and try again.";
  }
  if (!dynamic_control_->IsDynamicPartitionsEnabled()) {
    return true;
  }

  auto source_slot = GetCurrentSlot();
  if (target_slot == source_slot) {
    LOG(ERROR) << "Cannot call InitPartitionMetadata on current slot.";
    return false;
  }

  // Although the current build supports dynamic partitions, the given payload
  // doesn't use it for target partitions. This could happen when applying a
  // retrofit update. Skip updating the partition metadata for the target slot.
  is_target_dynamic_ = !partition_metadata.groups.empty();
  if (!is_target_dynamic_) {
    return true;
  }

  if (!update_metadata) {
    return true;
  }

  string target_suffix;
  if (!GetSuffix(target_slot, &target_suffix)) {
    return false;
  }

  // Unmap all the target dynamic partitions because they would become
  // inconsistent with the new metadata.
  if (!UnmapTargetPartitions(
          dynamic_control_.get(), target_suffix, partition_metadata)) {
    return false;
  }

  return UpdatePartitionMetadata(dynamic_control_.get(),
                                 source_slot,
                                 target_slot,
                                 target_suffix,
                                 partition_metadata);
}

}  // namespace chromeos_update_engine

