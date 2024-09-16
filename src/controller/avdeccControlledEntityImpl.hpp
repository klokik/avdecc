/*
* Copyright (C) 2016-2021, L-Acoustics and its contributors

* This file is part of LA_avdecc.

* LA_avdecc is free software: you can redistribute it and/or modify
* it under the terms of the GNU Lesser General Public License as published by
* the Free Software Foundation, either version 3 of the License, or
* (at your option) any later version.

* LA_avdecc is distributed in the hope that it will be useful,
* but WITHOUT ANY WARRANTY; without even the implied warranty of
* MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
* GNU Lesser General Public License for more details.

* You should have received a copy of the GNU Lesser General Public License
* along with LA_avdecc.  If not, see <http://www.gnu.org/licenses/>.
*/

/**
* @file avdeccControlledEntityImpl.hpp
* @author Christophe Calmejane
*/

#pragma once

#include <la/avdecc/internals/entityModelTree.hpp>

#include "la/avdecc/controller/internals/avdeccControlledEntity.hpp"

#include <string>
#include <unordered_map>
#include <unordered_set>
#include <bitset>
#include <functional>
#include <chrono>
#include <mutex>
#include <utility>
#include <thread>

namespace la
{
namespace avdecc
{
namespace controller
{
/* ************************************************************************** */
/* ControlledEntityImpl                                                       */
/* ************************************************************************** */
class ControlledEntityImpl : public ControlledEntity
{
public:
	/** Lock Information that is shared among all ControlledEntities */
	struct LockInformation
	{
		using SharedPointer = std::shared_ptr<LockInformation>;

		std::recursive_mutex _lock{};
		std::uint32_t _lockedCount{ 0u };
		std::thread::id _lockingThreadID{};

		void lock() noexcept
		{
			_lock.lock();
			if (_lockedCount == 0)
			{
				_lockingThreadID = std::this_thread::get_id();
			}
			++_lockedCount;
		}

		void unlock() noexcept
		{
			AVDECC_ASSERT(isSelfLocked(), "unlock should not be called when current thread is not the lock holder");

			--_lockedCount;
			if (_lockedCount == 0)
			{
				_lockingThreadID = {};
			}
			_lock.unlock();
		}

		void lockAll(std::uint32_t const lockedCount) noexcept
		{
			for (auto count = 0u; count < lockedCount; ++count)
			{
				lock();
			}
		}

		std::uint32_t unlockAll() noexcept
		{
			AVDECC_ASSERT(isSelfLocked(), "unlockAll should not be called when current thread is not the lock holder");

			auto result = 0u;
			[[maybe_unused]] auto const previousLockedCount = _lockedCount;
			while (isSelfLocked())
			{
				unlock();
				++result;
			}

			AVDECC_ASSERT(previousLockedCount == result, "lockedCount does not match the number of unlockings");
			return result;
		}

		bool isSelfLocked() const noexcept
		{
			return _lockingThreadID == std::this_thread::get_id();
		}
	};

	enum class EnumerationStep : std::uint16_t
	{
		GetMilanInfo = 1u << 0,
		RegisterUnsol = 1u << 1,
		GetStaticModel = 1u << 2,
		GetDescriptorDynamicInfo = 1u << 3, /** DescriptorDynamicInfoType */
		GetDynamicInfo = 1u << 4, /** DynamicInfoType */
	};
	using EnumerationSteps = utils::EnumBitfield<EnumerationStep>;

	/** Milan Vendor Unique Information */
	enum class MilanInfoType : std::uint16_t
	{
		MilanInfo, // GET_MILAN_INFO
	};

	/** Dynamic information to retrieve from entities. This is always required, either from a first enumeration or from recover from loss of unsolicited notification. */
	enum class DynamicInfoType : std::uint16_t
	{
		AcquiredState, // acquireEntity(ReleasedFlag)
		LockedState, // lockEntity(ReleasedFlag)
		InputStreamAudioMappings, // getStreamPortInputAudioMap (GET_AUDIO_MAP)
		OutputStreamAudioMappings, // getStreamPortOutputAudioMap (GET_AUDIO_MAP)
		InputStreamState, // getListenerStreamState (GET_RX_STATE)
		OutputStreamState, // getTalkerStreamState (GET_TX_STATE)
		OutputStreamConnection, // getTalkerStreamConnection (GET_TX_CONNECTION)
		InputStreamInfo, // getStreamInputInfo (GET_STREAM_INFO)
		OutputStreamInfo, // getStreamOutputInfo (GET_STREAM_INFO)
		GetAvbInfo, // getAvbInfo (GET_AVB_INFO)
		GetAsPath, // getAsPath (GET_AS_PATH)
		GetEntityCounters, // getEntityCounters (GET_COUNTERS)
		GetAvbInterfaceCounters, // getAvbInterfaceCounters (GET_COUNTERS)
		GetClockDomainCounters, // getClockDomainCounters (GET_COUNTERS)
		GetStreamInputCounters, // getStreamInputCounters (GET_COUNTERS)
		GetStreamOutputCounters, // getStreamOutputCounters (GET_COUNTERS)
	};

	/** Dynamic information stored in descriptors. Only required to retrieve from entities when the static model is known (because it was in EntityModelID cache).  */
	enum class DescriptorDynamicInfoType : std::uint16_t
	{
		ConfigurationName, // CONFIGURATION.object_name -> GET_NAME (7.4.18)
		AudioUnitName, // AUDIO_UNIT.object_name -> GET_NAME (7.4.18)
		AudioUnitSamplingRate, // AUDIO_UNIT.current_sampling_rate GET_SAMPLING_RATE (7.4.22)
		InputStreamName, // STREAM_INPUT.object_name -> GET_NAME (7.4.18)
		InputStreamFormat, // STREAM_INPUT.current_format -> GET_STREAM_FORMAT (7.4.10)
		OutputStreamName, // STREAM_OUTPUT.object_name -> GET_NAME (7.4.18)
		OutputStreamFormat, // STREAM_OUTPUT.current_format -> GET_STREAM_FORMAT (7.4.10)
		AvbInterfaceName, // AVB_INTERFACE.object_name -> GET_NAME (7.4.18)
		ClockSourceName, // CLOCK_SOURCE.object_name -> GET_NAME (7.4.18)
		MemoryObjectName, // MEMORY_OBJECT.object_name -> GET_NAME (7.4.18)
		MemoryObjectLength, // MEMORY_OBJECT.length -> GET_MEMORY_OBJECT_LENGTH (7.4.73)
		AudioClusterName, // AUDIO_CLUSTER.object_name -> GET_NAME (7.4.18)
		ControlName, // CONTROL.object_name -> GET_NAME (7.4.18)
		ControlValues, // CONTROL.value_details -> GET_CONTROL (7.4.26)
		ClockDomainName, // CLOCK_DOMAIN.object_name -> GET_NAME (7.4.18)
		ClockDomainSourceIndex, // CLOCK_DOMAIN.clock_source_index -> GET_CLOCK_SOURCE (7.4.24)
	};

	using MilanInfoKey = std::underlying_type_t<MilanInfoType>;
	using DescriptorKey = std::uint32_t;
	static_assert(sizeof(DescriptorKey) >= sizeof(entity::model::DescriptorType) + sizeof(entity::model::DescriptorIndex), "DescriptorKey size must be greater or equal to DescriptorType + DescriptorIndex");
	using DynamicInfoKey = std::uint64_t;
	static_assert(sizeof(DynamicInfoKey) >= sizeof(DynamicInfoType) + sizeof(entity::model::DescriptorIndex) + sizeof(std::uint16_t), "DynamicInfoKey size must be greater or equal to DynamicInfoType + DescriptorIndex + std::uint16_t");
	using DescriptorDynamicInfoKey = std::uint64_t;
	static_assert(sizeof(DescriptorDynamicInfoKey) >= sizeof(DescriptorDynamicInfoType) + sizeof(entity::model::DescriptorIndex), "DescriptorDynamicInfoKey size must be greater or equal to DescriptorDynamicInfoType + DescriptorIndex");

	/** Constructor */
	ControlledEntityImpl(la::avdecc::entity::Entity const& entity, LockInformation::SharedPointer const& sharedLock, bool const isVirtual) noexcept;

	// ControlledEntity overrides
	// Getters
	virtual bool isVirtual() const noexcept override;
	virtual CompatibilityFlags getCompatibilityFlags() const noexcept override;
	virtual bool gotFatalEnumerationError() const noexcept override;
	virtual bool isSubscribedToUnsolicitedNotifications() const noexcept override;
	virtual bool isAcquired() const noexcept override;
	virtual bool isAcquireCommandInProgress() const noexcept override;
	virtual bool isAcquiredByOther() const noexcept override;
	virtual bool isLocked() const noexcept override;
	virtual bool isLockCommandInProgress() const noexcept override;
	virtual bool isLockedByOther() const noexcept override;
	virtual bool isStreamInputRunning(entity::model::ConfigurationIndex const configurationIndex, entity::model::StreamIndex const streamIndex) const override;
	virtual bool isStreamOutputRunning(entity::model::ConfigurationIndex const configurationIndex, entity::model::StreamIndex const streamIndex) const override;
	virtual InterfaceLinkStatus getAvbInterfaceLinkStatus(entity::model::AvbInterfaceIndex const avbInterfaceIndex) const noexcept override;
	virtual model::AcquireState getAcquireState() const noexcept override;
	virtual UniqueIdentifier getOwningControllerID() const noexcept override;
	virtual model::LockState getLockState() const noexcept override;
	virtual UniqueIdentifier getLockingControllerID() const noexcept override;
	virtual entity::Entity const& getEntity() const noexcept override;
	virtual std::optional<entity::model::MilanInfo> getMilanInfo() const noexcept override;
	virtual std::optional<entity::model::ControlIndex> getIdentifyControlIndex() const noexcept override;
	virtual bool isEntityModelValidForCaching() const noexcept override;
	virtual bool isIdentifying() const noexcept override;

	virtual model::EntityNode const& getEntityNode() const override;
	virtual model::ConfigurationNode const& getConfigurationNode(entity::model::ConfigurationIndex const configurationIndex) const override;
	virtual model::ConfigurationNode const& getCurrentConfigurationNode() const override;
	virtual model::StreamInputNode const& getStreamInputNode(entity::model::ConfigurationIndex const configurationIndex, entity::model::StreamIndex const streamIndex) const override;
	virtual model::StreamOutputNode const& getStreamOutputNode(entity::model::ConfigurationIndex const configurationIndex, entity::model::StreamIndex const streamIndex) const override;
#ifdef ENABLE_AVDECC_FEATURE_REDUNDANCY
	virtual model::RedundantStreamNode const& getRedundantStreamInputNode(entity::model::ConfigurationIndex const configurationIndex, model::VirtualIndex const redundantStreamIndex) const override;
	virtual model::RedundantStreamNode const& getRedundantStreamOutputNode(entity::model::ConfigurationIndex const configurationIndex, model::VirtualIndex const redundantStreamIndex) const override;
#endif // ENABLE_AVDECC_FEATURE_REDUNDANCY
	virtual model::AudioUnitNode const& getAudioUnitNode(entity::model::ConfigurationIndex const configurationIndex, entity::model::AudioUnitIndex const audioUnitIndex) const override;
	virtual model::AvbInterfaceNode const& getAvbInterfaceNode(entity::model::ConfigurationIndex const configurationIndex, entity::model::AvbInterfaceIndex const avbInterfaceIndex) const override;
	virtual model::ClockSourceNode const& getClockSourceNode(entity::model::ConfigurationIndex const configurationIndex, entity::model::ClockSourceIndex const clockSourceIndex) const override;
	virtual model::StreamPortNode const& getStreamPortInputNode(entity::model::ConfigurationIndex const configurationIndex, entity::model::StreamPortIndex const streamPortIndex) const override;
	virtual model::StreamPortNode const& getStreamPortOutputNode(entity::model::ConfigurationIndex const configurationIndex, entity::model::StreamPortIndex const streamPortIndex) const override;
	//virtual model::AudioClusterNode const& getAudioClusterNode(entity::model::ConfigurationIndex const configurationIndex, entity::model::ClusterIndex const clusterIndex) const override;
	//virtual model::AudioMapNode const& getAudioMapNode(entity::model::ConfigurationIndex const configurationIndex, entity::model::MapIndex const mapIndex) const override;
	virtual model::ControlNode const& getControlNode(entity::model::ConfigurationIndex const configurationIndex, entity::model::ControlIndex const controlIndex) const override;
	virtual model::ClockDomainNode const& getClockDomainNode(entity::model::ConfigurationIndex const configurationIndex, entity::model::ClockDomainIndex const clockDomainIndex) const override;

	virtual entity::model::LocaleNodeStaticModel const* findLocaleNode(entity::model::ConfigurationIndex const configurationIndex, std::string const& locale) const override; // Throws Exception::NotSupported if EM not supported by the Entity // Throws Exception::InvalidConfigurationIndex if configurationIndex do not exist
	virtual entity::model::AvdeccFixedString const& getLocalizedString(entity::model::LocalizedStringReference const& stringReference) const noexcept override;
	virtual entity::model::AvdeccFixedString const& getLocalizedString(entity::model::ConfigurationIndex const configurationIndex, entity::model::LocalizedStringReference const& stringReference) const noexcept override; // Get localized string or empty string if not found // Throws Exception::InvalidConfigurationIndex if configurationIndex do not exist

	// Visitor method
	virtual void accept(model::EntityModelVisitor* const visitor, bool const visitAllConfigurations = false) const noexcept override;

	virtual void lock() noexcept override;
	virtual void unlock() noexcept override;

	virtual entity::model::StreamInputConnectionInfo const& getSinkConnectionInformation(entity::model::StreamIndex const streamIndex) const override; // Throws Exception::InvalidDescriptorIndex if streamIndex do not exist
	virtual entity::model::AudioMappings const& getStreamPortInputAudioMappings(entity::model::StreamPortIndex const streamPortIndex) const override; // Throws Exception::InvalidDescriptorIndex if streamPortIndex do not exist
	virtual entity::model::AudioMappings getStreamPortInputNonRedundantAudioMappings(entity::model::StreamPortIndex const streamPortIndex) const override; // Throws Exception::InvalidDescriptorIndex if streamPortIndex do not exist
	virtual entity::model::AudioMappings const& getStreamPortOutputAudioMappings(entity::model::StreamPortIndex const streamPortIndex) const override; // Throws Exception::InvalidDescriptorIndex if streamPortIndex do not exist
	virtual entity::model::AudioMappings getStreamPortOutputNonRedundantAudioMappings(entity::model::StreamPortIndex const streamPortIndex) const override; // Throws Exception::InvalidDescriptorIndex if streamPortIndex do not exist

	/** Get connections information about a talker's stream */
	virtual entity::model::StreamConnections const& getStreamOutputConnections(entity::model::StreamIndex const streamIndex) const override; // Throws Exception::InvalidDescriptorIndex if streamIndex do not exist

	// Statistics
	virtual std::uint64_t getAecpRetryCounter() const noexcept override;
	virtual std::uint64_t getAecpTimeoutCounter() const noexcept override;
	virtual std::uint64_t getAecpUnexpectedResponseCounter() const noexcept override;
	virtual std::chrono::milliseconds const& getAecpResponseAverageTime() const noexcept override;
	virtual std::uint64_t getAemAecpUnsolicitedCounter() const noexcept override;
	virtual std::chrono::milliseconds const& getEnumerationTime() const noexcept override;

	// Const Tree getters, all throw Exception::NotSupported if EM not supported by the Entity, Exception::InvalidConfigurationIndex if configurationIndex do not exist
	entity::model::EntityTree const& getEntityTree() const;
	entity::model::ConfigurationTree const& getConfigurationTree(entity::model::ConfigurationIndex const configurationIndex) const;
	entity::model::ConfigurationIndex getCurrentConfigurationIndex() const noexcept;

	// Const NodeModel getters, all throw Exception::NotSupported if EM not supported by the Entity, Exception::InvalidConfigurationIndex if configurationIndex do not exist, Exception::InvalidDescriptorIndex if descriptorIndex is invalid
	entity::model::EntityNodeStaticModel const& getEntityNodeStaticModel() const;
	entity::model::EntityNodeDynamicModel const& getEntityNodeDynamicModel() const;
	entity::model::ConfigurationNodeStaticModel const& getConfigurationNodeStaticModel(entity::model::ConfigurationIndex const configurationIndex) const;
	entity::model::ConfigurationNodeDynamicModel const& getConfigurationNodeDynamicModel(entity::model::ConfigurationIndex const configurationIndex) const;
	template<typename FieldPointer, typename DescriptorIndexType>
	auto const& getNodeStaticModel(entity::model::ConfigurationIndex const configurationIndex, DescriptorIndexType const index, FieldPointer entity::model::ConfigurationTree::*Field) const
	{
		auto const& configTree = getConfigurationTree(configurationIndex);

		auto const it = (configTree.*Field).find(index);
		if (it == (configTree.*Field).end())
			throw Exception(Exception::Type::InvalidDescriptorIndex, "Invalid index");

		return it->second.staticModel;
	}
	template<typename FieldPointer, typename DescriptorIndexType>
	auto const& getNodeDynamicModel(entity::model::ConfigurationIndex const configurationIndex, DescriptorIndexType const index, FieldPointer entity::model::ConfigurationTree::*Field) const
	{
		auto const& configTree = getConfigurationTree(configurationIndex);

		auto const it = (configTree.*Field).find(index);
		if (it == (configTree.*Field).end())
			throw Exception(Exception::Type::InvalidDescriptorIndex, "Invalid index");

		return it->second.dynamicModel;
	}

	// Tree validators, to check if a specific part exists yet without throwing
	bool hasAnyConfigurationTree() const noexcept;
	bool hasConfigurationTree(entity::model::ConfigurationIndex const configurationIndex) const noexcept;
	template<typename FieldPointer, typename DescriptorIndexType>
	bool hasTreeModel(entity::model::ConfigurationIndex const configurationIndex, DescriptorIndexType const index, FieldPointer entity::model::ConfigurationTree::*Field) const noexcept
	{
		AVDECC_ASSERT(_sharedLock->_lockedCount >= 0, "ControlledEntity should be locked");

		if (gotFatalEnumerationError() || !_entity.getEntityCapabilities().test(entity::EntityCapability::AemSupported))
		{
			return false;
		}

		if (auto const configIt = _entityTree.configurationTrees.find(configurationIndex); configIt != _entityTree.configurationTrees.end())
		{
			auto const& configTree = configIt->second;
			return (configTree.*Field).find(index) != (configTree.*Field).end();
		}

		return false;
	}

	// Non-const Tree getters
	entity::model::EntityTree& getEntityTree() noexcept;
	entity::model::ConfigurationTree& getConfigurationTree(entity::model::ConfigurationIndex const configurationIndex) noexcept;

	// Non-const NodeModel getters
	entity::model::EntityNodeStaticModel& getEntityNodeStaticModel() noexcept;
	entity::model::EntityNodeDynamicModel& getEntityNodeDynamicModel() noexcept;
	entity::model::ConfigurationNodeStaticModel& getConfigurationNodeStaticModel(entity::model::ConfigurationIndex const configurationIndex) noexcept;
	entity::model::ConfigurationNodeDynamicModel& getConfigurationNodeDynamicModel(entity::model::ConfigurationIndex const configurationIndex) noexcept;
	template<typename FieldPointer, typename DescriptorIndexType>
	auto& getNodeStaticModel(entity::model::ConfigurationIndex const configurationIndex, DescriptorIndexType const index, FieldPointer entity::model::ConfigurationTree::*Field) noexcept
	{
		AVDECC_ASSERT(_sharedLock->_lockedCount >= 0, "ControlledEntity should be locked");

		auto& configTree = getConfigurationTree(configurationIndex);
		return (configTree.*Field)[index].staticModel;
	}
	template<typename FieldPointer, typename DescriptorIndexType>
	auto& getNodeDynamicModel(entity::model::ConfigurationIndex const configurationIndex, DescriptorIndexType const index, FieldPointer entity::model::ConfigurationTree::*Field) noexcept
	{
		AVDECC_ASSERT(_sharedLock->_lockedCount >= 0, "ControlledEntity should be locked");

		auto& configTree = getConfigurationTree(configurationIndex);
		return (configTree.*Field)[index].dynamicModel;
	}
	template<typename FieldPointer>
	auto& getModels(entity::model::ConfigurationIndex const configurationIndex, FieldPointer entity::model::ConfigurationTree::*Field) noexcept
	{
		AVDECC_ASSERT(_sharedLock->_lockedCount >= 0, "ControlledEntity should be locked");

		auto& entityTree = getEntityTree();
		auto configIt = entityTree.configurationTrees.find(configurationIndex);
		if (configIt != entityTree.configurationTrees.end())
		{
			auto& configTree = configIt->second;
			return configTree.*Field;
		}

		// We return a reference, so we have to create a static empty model in case we don't have one so we can return a reference to it
		static auto s_Empty = FieldPointer{};
		return s_Empty;
	}
	entity::model::EntityCounters& getEntityCounters() noexcept;
	entity::model::AvbInterfaceCounters& getAvbInterfaceCounters(entity::model::AvbInterfaceIndex const avbInterfaceIndex) noexcept;
	entity::model::ClockDomainCounters& getClockDomainCounters(entity::model::ClockDomainIndex const clockDomainIndex) noexcept;
	entity::model::StreamInputCounters& getStreamInputCounters(entity::model::StreamIndex const streamIndex) noexcept;
	entity::model::StreamOutputCounters& getStreamOutputCounters(entity::model::StreamIndex const streamIndex) noexcept;

	// Setters of the DescriptorDynamic info, default constructing if not existing
	void setEntityName(entity::model::AvdeccFixedString const& name) noexcept;
	void setEntityGroupName(entity::model::AvdeccFixedString const& name) noexcept;
	void setCurrentConfiguration(entity::model::ConfigurationIndex const configurationIndex) noexcept;
	void setConfigurationName(entity::model::ConfigurationIndex const configurationIndex, entity::model::AvdeccFixedString const& name) noexcept;
	template<typename FieldPointer, typename DescriptorIndexType>
	void setObjectName(entity::model::ConfigurationIndex const configurationIndex, DescriptorIndexType const index, FieldPointer entity::model::ConfigurationTree::*Field, entity::model::AvdeccFixedString const& name) noexcept
	{
		auto& dynamicModel = getNodeDynamicModel(configurationIndex, index, Field);
		dynamicModel.objectName = name;
	}
	void setSamplingRate(entity::model::AudioUnitIndex const audioUnitIndex, entity::model::SamplingRate const samplingRate) noexcept;
	entity::model::StreamInputConnectionInfo setStreamInputConnectionInformation(entity::model::StreamIndex const streamIndex, entity::model::StreamInputConnectionInfo const& info) noexcept;
	void clearStreamOutputConnections(entity::model::StreamIndex const streamIndex) noexcept;
	bool addStreamOutputConnection(entity::model::StreamIndex const streamIndex, entity::model::StreamIdentification const& listenerStream) noexcept; // Returns true if effectively added
	bool delStreamOutputConnection(entity::model::StreamIndex const streamIndex, entity::model::StreamIdentification const& listenerStream) noexcept; // Returns true if effectively removed
	entity::model::AvbInterfaceInfo setAvbInterfaceInfo(entity::model::AvbInterfaceIndex const avbInterfaceIndex, entity::model::AvbInterfaceInfo const& info) noexcept; // Returns previous AvbInterfaceInfo
	entity::model::AsPath setAsPath(entity::model::AvbInterfaceIndex const avbInterfaceIndex, entity::model::AsPath const& asPath) noexcept; // Returns previous AsPath
	void setSelectedLocaleStringsIndexesRange(entity::model::ConfigurationIndex const configurationIndex, entity::model::StringsIndex const baseIndex, entity::model::StringsIndex const countIndexes) noexcept;
	void clearStreamPortInputAudioMappings(entity::model::StreamPortIndex const streamPortIndex) noexcept;
	void addStreamPortInputAudioMappings(entity::model::StreamPortIndex const streamPortIndex, entity::model::AudioMappings const& mappings) noexcept;
	void removeStreamPortInputAudioMappings(entity::model::StreamPortIndex const streamPortIndex, entity::model::AudioMappings const& mappings) noexcept;
	void clearStreamPortOutputAudioMappings(entity::model::StreamPortIndex const streamPortIndex) noexcept;
	void addStreamPortOutputAudioMappings(entity::model::StreamPortIndex const streamPortIndex, entity::model::AudioMappings const& mappings) noexcept;
	void removeStreamPortOutputAudioMappings(entity::model::StreamPortIndex const streamPortIndex, entity::model::AudioMappings const& mappings) noexcept;
	void setClockSource(entity::model::ClockDomainIndex const clockDomainIndex, entity::model::ClockSourceIndex const clockSourceIndex) noexcept;
	void setControlValues(entity::model::ControlIndex const controlIndex, entity::model::ControlValues const& controlValues) noexcept;
	void setMemoryObjectLength(entity::model::ConfigurationIndex const configurationIndex, entity::model::MemoryObjectIndex const memoryObjectIndex, std::uint64_t const length) noexcept;

	// Setters of the global state
	void setEntity(entity::Entity const& entity) noexcept;
	InterfaceLinkStatus setAvbInterfaceLinkStatus(entity::model::AvbInterfaceIndex const avbInterfaceIndex, InterfaceLinkStatus const linkStatus) noexcept; // Returns previous link status
	void setAcquireState(model::AcquireState const state) noexcept;
	void setOwningController(UniqueIdentifier const controllerID) noexcept;
	void setLockState(model::LockState const state) noexcept;
	void setLockingController(UniqueIdentifier const controllerID) noexcept;
	void setMilanInfo(entity::model::MilanInfo const& info) noexcept;

	// Setters of the Statistics
	void setAecpRetryCounter(std::uint64_t const value) noexcept;
	void setAecpTimeoutCounter(std::uint64_t const value) noexcept;
	void setAecpUnexpectedResponseCounter(std::uint64_t const value) noexcept;
	void setAecpResponseAverageTime(std::chrono::milliseconds const& value) noexcept;
	void setAemAecpUnsolicitedCounter(std::uint64_t const value) noexcept;
	void setEnumerationTime(std::chrono::milliseconds const& value) noexcept;

	// Setters of the Model from AEM Descriptors (including DescriptorDynamic info)
	void setEntityTree(entity::model::EntityTree const& entityTree) noexcept;
	bool setCachedEntityTree(entity::model::EntityTree const& cachedTree, entity::model::EntityDescriptor const& descriptor, bool const forAllConfiguration) noexcept; // Returns true if the cached EntityTree is accepted (and set) for this entity
	void setEntityDescriptor(entity::model::EntityDescriptor const& descriptor) noexcept;
	void setConfigurationDescriptor(entity::model::ConfigurationDescriptor const& descriptor, entity::model::ConfigurationIndex const configurationIndex) noexcept;
	void setAudioUnitDescriptor(entity::model::AudioUnitDescriptor const& descriptor, entity::model::ConfigurationIndex const configurationIndex, entity::model::AudioUnitIndex const audioUnitIndex) noexcept;
	void setStreamInputDescriptor(entity::model::StreamDescriptor const& descriptor, entity::model::ConfigurationIndex const configurationIndex, entity::model::StreamIndex const streamIndex) noexcept;
	void setStreamOutputDescriptor(entity::model::StreamDescriptor const& descriptor, entity::model::ConfigurationIndex const configurationIndex, entity::model::StreamIndex const streamIndex) noexcept;
	void setAvbInterfaceDescriptor(entity::model::AvbInterfaceDescriptor const& descriptor, entity::model::ConfigurationIndex const configurationIndex, entity::model::AvbInterfaceIndex const interfaceIndex) noexcept;
	void setClockSourceDescriptor(entity::model::ClockSourceDescriptor const& descriptor, entity::model::ConfigurationIndex const configurationIndex, entity::model::ClockSourceIndex const clockIndex) noexcept;
	void setMemoryObjectDescriptor(entity::model::MemoryObjectDescriptor const& descriptor, entity::model::ConfigurationIndex const configurationIndex, entity::model::MemoryObjectIndex const memoryObjectIndex) noexcept;
	void setLocaleDescriptor(entity::model::LocaleDescriptor const& descriptor, entity::model::ConfigurationIndex const configurationIndex, entity::model::LocaleIndex const localeIndex) noexcept;
	void setStringsDescriptor(entity::model::StringsDescriptor const& descriptor, entity::model::ConfigurationIndex const configurationIndex, entity::model::StringsIndex const stringsIndex) noexcept;
	void setLocalizedStrings(entity::model::ConfigurationIndex const configurationIndex, entity::model::StringsIndex const relativeStringsIndex, entity::model::AvdeccFixedStrings const& strings) noexcept;
	void setStreamPortInputDescriptor(entity::model::StreamPortDescriptor const& descriptor, entity::model::ConfigurationIndex const configurationIndex, entity::model::StreamPortIndex const streamPortIndex) noexcept;
	void setStreamPortOutputDescriptor(entity::model::StreamPortDescriptor const& descriptor, entity::model::ConfigurationIndex const configurationIndex, entity::model::StreamPortIndex const streamPortIndex) noexcept;
	void setAudioClusterDescriptor(entity::model::AudioClusterDescriptor const& descriptor, entity::model::ConfigurationIndex const configurationIndex, entity::model::ClusterIndex const clusterIndex) noexcept;
	void setAudioMapDescriptor(entity::model::AudioMapDescriptor const& descriptor, entity::model::ConfigurationIndex const configurationIndex, entity::model::MapIndex const mapIndex) noexcept;
	void setControlDescriptor(entity::model::ControlDescriptor const& descriptor, entity::model::ConfigurationIndex const configurationIndex, entity::model::ControlIndex const controlIndex) noexcept;
	void setClockDomainDescriptor(entity::model::ClockDomainDescriptor const& descriptor, entity::model::ConfigurationIndex const configurationIndex, entity::model::ClockDomainIndex const clockDomainIndex) noexcept;

	// Setters of statistics
	std::uint64_t incrementAecpRetryCounter() noexcept;
	std::uint64_t incrementAecpTimeoutCounter() noexcept;
	std::uint64_t incrementAecpUnexpectedResponseCounter() noexcept;
	std::chrono::milliseconds const& updateAecpResponseTimeAverage(std::chrono::milliseconds const& responseTime) noexcept;
	std::uint64_t incrementAemAecpUnsolicitedCounter() noexcept;
	void setStartEnumerationTime(std::chrono::time_point<std::chrono::steady_clock>&& startTime) noexcept;
	void setEndEnumerationTime(std::chrono::time_point<std::chrono::steady_clock>&& endTime) noexcept;

	// Expected RegisterUnsol query methods
	bool checkAndClearExpectedRegisterUnsol() noexcept;
	void setRegisterUnsolExpected() noexcept;
	bool gotExpectedRegisterUnsol() const noexcept;
	std::pair<bool, std::chrono::milliseconds> getRegisterUnsolRetryTimer() noexcept;

	// Expected Milan info query methods
	bool checkAndClearExpectedMilanInfo(MilanInfoType const milanInfoType) noexcept;
	void setMilanInfoExpected(MilanInfoType const milanInfoType) noexcept;
	bool gotAllExpectedMilanInfo() const noexcept;
	std::pair<bool, std::chrono::milliseconds> getQueryMilanInfoRetryTimer() noexcept;

	// Expected descriptor query methods
	bool checkAndClearExpectedDescriptor(entity::model::ConfigurationIndex const configurationIndex, entity::model::DescriptorType const descriptorType, entity::model::DescriptorIndex const descriptorIndex) noexcept;
	void setDescriptorExpected(entity::model::ConfigurationIndex const configurationIndex, entity::model::DescriptorType const descriptorType, entity::model::DescriptorIndex const descriptorIndex) noexcept;
	bool gotAllExpectedDescriptors() const noexcept;
	std::pair<bool, std::chrono::milliseconds> getQueryDescriptorRetryTimer() noexcept;

	// Expected dynamic info query methods
	bool checkAndClearExpectedDynamicInfo(entity::model::ConfigurationIndex const configurationIndex, DynamicInfoType const dynamicInfoType, entity::model::DescriptorIndex const descriptorIndex, std::uint16_t const subIndex = 0u) noexcept;
	void setDynamicInfoExpected(entity::model::ConfigurationIndex const configurationIndex, DynamicInfoType const dynamicInfoType, entity::model::DescriptorIndex const descriptorIndex, std::uint16_t const subIndex = 0u) noexcept;
	bool gotAllExpectedDynamicInfo() const noexcept;
	std::pair<bool, std::chrono::milliseconds> getQueryDynamicInfoRetryTimer() noexcept;

	// Expected descriptor dynamic info query methods
	bool checkAndClearExpectedDescriptorDynamicInfo(entity::model::ConfigurationIndex const configurationIndex, DescriptorDynamicInfoType const descriptorDynamicInfoType, entity::model::DescriptorIndex const descriptorIndex) noexcept;
	void setDescriptorDynamicInfoExpected(entity::model::ConfigurationIndex const configurationIndex, DescriptorDynamicInfoType const descriptorDynamicInfoType, entity::model::DescriptorIndex const descriptorIndex) noexcept;
	void clearAllExpectedDescriptorDynamicInfo() noexcept;
	bool gotAllExpectedDescriptorDynamicInfo() const noexcept;
	std::pair<bool, std::chrono::milliseconds> getQueryDescriptorDynamicInfoRetryTimer() noexcept;

	// Other getters/setters
	entity::Entity& getEntity() noexcept;
	void setIdentifyControlIndex(entity::model::ControlIndex const identifyControlIndex) noexcept;
	bool shouldIgnoreCachedEntityModel() const noexcept;
	void setIgnoreCachedEntityModel() noexcept;
	EnumerationSteps getEnumerationSteps() const noexcept;
	void setEnumerationSteps(EnumerationSteps const steps) noexcept;
	void addEnumerationStep(EnumerationStep const step) noexcept;
	void clearEnumerationStep(EnumerationStep const step) noexcept;
	void setCompatibilityFlags(CompatibilityFlags const compatibilityFlags) noexcept;
	void setGetFatalEnumerationError() noexcept;
	void setSubscribedToUnsolicitedNotifications(bool const isSubscribed) noexcept;
	bool wasAdvertised() const noexcept;
	void setAdvertised(bool const wasAdvertised) noexcept;
	bool isRedundantPrimaryStreamInput(entity::model::StreamIndex const streamIndex) const noexcept; // True for a Redundant Primary Stream (false for Secondary and non-redundant streams)
	bool isRedundantPrimaryStreamOutput(entity::model::StreamIndex const streamIndex) const noexcept; // True for a Redundant Primary Stream (false for Secondary and non-redundant streams)
	bool isRedundantSecondaryStreamInput(entity::model::StreamIndex const streamIndex) const noexcept; // True for a Redundant Secondary Stream (false for Primary and non-redundant streams)
	bool isRedundantSecondaryStreamOutput(entity::model::StreamIndex const streamIndex) const noexcept; // True for a Redundant Secondary Stream (false for Primary and non-redundant streams)

	// Static methods
	static std::string dynamicInfoTypeToString(DynamicInfoType const dynamicInfoType) noexcept;
	static std::string descriptorDynamicInfoTypeToString(DescriptorDynamicInfoType const descriptorDynamicInfoType) noexcept;

	// Controller restricted methods
	void onEntityFullyLoaded() noexcept; // To be called when the entity has been fully loaded and is ready to be shared

	// Compiler auto-generated methods
	ControlledEntityImpl(ControlledEntityImpl&&) = delete;
	ControlledEntityImpl(ControlledEntityImpl const&) = delete;
	ControlledEntityImpl& operator=(ControlledEntityImpl const&) = delete;
	ControlledEntityImpl& operator=(ControlledEntityImpl&&) = delete;

protected:
	using RedundantStreamCategory = std::unordered_set<entity::model::StreamIndex>;

	template<class NodeType, typename = std::enable_if_t<std::is_base_of<model::Node, NodeType>::value>>
	static constexpr size_t getHashCode(NodeType const* const node) noexcept
	{
		return typeid(decltype(node)).hash_code();
	}
	template<class NodeType, typename = std::enable_if_t<std::is_base_of<model::Node, NodeType>::value>>
	static constexpr size_t getHashCode() noexcept
	{
		return typeid(NodeType const*).hash_code();
	}

	template<class NodeType, typename = std::enable_if_t<std::is_base_of<model::VirtualNode, NodeType>::value>>
	static void initNode(NodeType& node, entity::model::DescriptorType const descriptorType) noexcept
	{
		node.descriptorType = descriptorType;
	}

	template<class NodeType, typename = std::enable_if_t<std::is_base_of<model::EntityModelNode, NodeType>::value>>
	static void initNode(NodeType& node, entity::model::DescriptorType const descriptorType, entity::model::DescriptorIndex const descriptorIndex) noexcept
	{
		node.descriptorType = descriptorType;
		node.descriptorIndex = descriptorIndex;
	}

	template<class NodeType, typename = std::enable_if_t<std::is_base_of<model::VirtualNode, NodeType>::value>>
	static void initNode(NodeType& node, entity::model::DescriptorType const descriptorType, model::VirtualIndex const virtualIndex) noexcept
	{
		node.descriptorType = descriptorType;
		node.virtualIndex = virtualIndex;
	}

private:
	// Private methods
	void buildEntityModelGraph() noexcept;
	bool isEntityModelComplete(entity::model::EntityTree const& entityTree, std::uint16_t const configurationsCount) const noexcept;
#ifdef ENABLE_AVDECC_FEATURE_REDUNDANCY
	void buildRedundancyNodes(model::ConfigurationNode& configNode) noexcept;
#endif // ENABLE_AVDECC_FEATURE_REDUNDANCY

	// Private variables
	LockInformation::SharedPointer _sharedLock{ nullptr };
	bool const _isVirtual{ false };
	bool _ignoreCachedEntityModel{ false };
	std::optional<entity::model::ControlIndex> _identifyControlIndex{ std::nullopt };
	std::uint16_t _registerUnsolRetryCount{ 0u };
	std::uint16_t _queryMilanInfoRetryCount{ 0u };
	std::uint16_t _queryDescriptorRetryCount{ 0u };
	std::uint16_t _queryDynamicInfoRetryCount{ 0u };
	std::uint16_t _queryDescriptorDynamicInfoRetryCount{ 0u };
	EnumerationSteps _enumerationSteps{};
	CompatibilityFlags _compatibilityFlags{ CompatibilityFlag::IEEE17221 }; // Entity is IEEE1722.1 compatible by default
	bool _gotFatalEnumerateError{ false }; // Have we got a fatal error during entity enumeration
	bool _isSubscribedToUnsolicitedNotifications{ false }; // Are we subscribed to unsolicited notifications
	bool _advertised{ false }; // Has the entity been advertised to the observers
	bool _expectedRegisterUnsol{ false };
	std::unordered_set<MilanInfoKey> _expectedMilanInfo{};
	std::unordered_map<entity::model::ConfigurationIndex, std::unordered_set<DescriptorKey>> _expectedDescriptors{};
	std::unordered_map<entity::model::ConfigurationIndex, std::unordered_set<DynamicInfoKey>> _expectedDynamicInfo{};
	std::unordered_map<entity::model::ConfigurationIndex, std::unordered_set<DescriptorDynamicInfoKey>> _expectedDescriptorDynamicInfo{};
	std::unordered_map<entity::model::AvbInterfaceIndex, InterfaceLinkStatus> _avbInterfaceLinkStatus{}; // Link status for each AvbInterface (true = up or unknown, false = down)
	model::AcquireState _acquireState{ model::AcquireState::Undefined };
	UniqueIdentifier _owningControllerID{}; // EID of the controller currently owning (who acquired) this entity
	model::LockState _lockState{ model::LockState::Undefined };
	UniqueIdentifier _lockingControllerID{}; // EID of the controller currently locking (who locked) this entity
	// Milan specific information
	std::optional<entity::model::MilanInfo> _milanInfo{ std::nullopt };
	// Entity variables
	entity::Entity _entity; // No NSMI, Entity has no default constructor but it has to be passed to the only constructor of this class anyway
	// Entity Model
	entity::model::EntityTree _entityTree{}; // Tree of the model as represented by the AVDECC protocol
	model::EntityNode _entityNode{}; // Model as represented by the ControlledEntity (tree of references to the model::EntityStaticTree and model::EntityDynamicTree)
	// Cached Information
	RedundantStreamCategory _redundantPrimaryStreamInputs{}; // Cached indexes of all Redundant Primary Streams (a non-redundant stream won't be listed here)
	RedundantStreamCategory _redundantPrimaryStreamOutputs{}; // Cached indexes of all Redundant Primary Streams (a non-redundant stream won't be listed here)
	RedundantStreamCategory _redundantSecondaryStreamInputs{}; // Cached indexes of all Redundant Secondary Streams
	RedundantStreamCategory _redundantSecondaryStreamOutputs{}; // Cached indexes of all Redundant Secondary Streams
	// Statistics
	std::uint64_t _aecpRetryCounter{ 0ull };
	std::uint64_t _aecpTimeoutCounter{ 0ull };
	std::uint64_t _aecpUnexpectedResponseCounter{ 0ull };
	std::uint64_t _aecpResponsesCount{ 0ull }; // Intermediate variable used by _aecpResponseAverageTime
	std::chrono::milliseconds _aecpResponseTimeSum{}; // Intermediate variable used by _aecpResponseAverageTime
	std::chrono::milliseconds _aecpResponseAverageTime{};
	std::uint64_t _aemAecpUnsolicitedCounter{ 0ull };
	std::chrono::time_point<std::chrono::steady_clock> _enumerationStartTime{}; // Intermediate variable used by _enumerationTime
	std::chrono::milliseconds _enumerationTime{};
};

} // namespace controller
} // namespace avdecc
} // namespace la
