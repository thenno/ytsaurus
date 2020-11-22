#include "node_directory.h"
#include "private.h"

#include <yt/client/node_tracker_client/node_directory.h>

#include <yt/client/node_tracker_client/proto/node_directory.pb.h>

#include <yt/core/concurrency/thread_affinity.h>

#include <yt/core/yson/consumer.h>

#include <yt/core/ytree/fluent.h>

#include <yt/core/net/address.h>

#include <yt/core/misc/protobuf_helpers.h>
#include <yt/core/misc/string.h>
#include <yt/core/misc/hash.h>
#include <yt/core/misc/small_vector.h>

#include <util/digest/numeric.h>

namespace NYT::NNodeTrackerClient {

using namespace NChunkClient;
using namespace NYson;
using namespace NYTree;
using namespace NNet;
using namespace NRpc;
using namespace NConcurrency;

using NYT::FromProto;
using NYT::ToProto;

////////////////////////////////////////////////////////////////////////////////

const TString& NullNodeAddress()
{
    static const TString Result("<null>");
    return Result;
}

const TNodeDescriptor& NullNodeDescriptor()
{
    static const TNodeDescriptor Result(NullNodeAddress());
    return Result;
}

////////////////////////////////////////////////////////////////////////////////

namespace {

constexpr int TypicalTagCount = 16;

// Cf. YT-10645
SmallVector<TStringBuf, TypicalTagCount> GetSortedTags(const std::vector<TString>& tags)
{
    SmallVector<TStringBuf, TypicalTagCount> result;
    result.reserve(tags.size());
    for (const auto& tag : tags) {
        result.push_back(tag);
    }
    std::sort(result.begin(), result.end());
    return result;
}

} // namespace

////////////////////////////////////////////////////////////////////////////////

TNodeDescriptor::TNodeDescriptor()
    : DefaultAddress_(NullNodeAddress())
{ }

TNodeDescriptor::TNodeDescriptor(const TString& defaultAddress)
    : Addresses_{std::make_pair(DefaultNetworkName, defaultAddress)}
    , DefaultAddress_(defaultAddress)
{ }

TNodeDescriptor::TNodeDescriptor(const std::optional<TString>& defaultAddress)
{
    if (defaultAddress) {
        *this = TNodeDescriptor(*defaultAddress);
    }
}

TNodeDescriptor::TNodeDescriptor(
    TAddressMap addresses,
    std::optional<TString> rack,
    std::optional<TString> dc,
    const std::vector<TString>& tags)
    : Addresses_(std::move(addresses))
    , DefaultAddress_(NNodeTrackerClient::GetDefaultAddress(Addresses_))
    , Rack_(std::move(rack))
    , DataCenter_(std::move(dc))
    , Tags_(tags)
{ }

bool TNodeDescriptor::IsNull() const
{
    return Addresses_.empty();
}

const TAddressMap& TNodeDescriptor::Addresses() const
{
    return Addresses_;
}

const TString& TNodeDescriptor::GetDefaultAddress() const
{
    return DefaultAddress_;
}

TAddressWithNetwork TNodeDescriptor::GetAddressWithNetworkOrThrow(const TNetworkPreferenceList& networks) const
{
    return NNodeTrackerClient::GetAddressWithNetworkOrThrow(Addresses(), networks);
}

const TString& TNodeDescriptor::GetAddressOrThrow(const TNetworkPreferenceList& networks) const
{
    return NNodeTrackerClient::GetAddressOrThrow(Addresses(), networks);
}

std::optional<TString> TNodeDescriptor::FindAddress(const TNetworkPreferenceList& networks) const
{
    return NNodeTrackerClient::FindAddress(Addresses(), networks);
}

const std::optional<TString>& TNodeDescriptor::GetRack() const
{
    return Rack_;
}

const std::optional<TString>& TNodeDescriptor::GetDataCenter() const
{
    return DataCenter_;
}

const std::vector<TString>& TNodeDescriptor::GetTags() const
{
    return Tags_;
}

void TNodeDescriptor::Persist(const TStreamPersistenceContext& context)
{
    using NYT::Persist;
    Persist(context, Addresses_);
    if (context.IsLoad()) {
        DefaultAddress_ = NNodeTrackerClient::GetDefaultAddress(Addresses_);
    }
    Persist(context, Rack_);
    Persist(context, DataCenter_);
}

void FormatValue(TStringBuilderBase* builder, const TNodeDescriptor& descriptor, TStringBuf /*spec*/)
{
    if (descriptor.IsNull()) {
        builder->AppendString(NullNodeAddress());
        return;
    }

    builder->AppendString(descriptor.GetDefaultAddress());
    if (const auto& rack = descriptor.GetRack()) {
        builder->AppendChar('@');
        builder->AppendString(*rack);
    }
    if (const auto& dataCenter = descriptor.GetDataCenter()) {
        builder->AppendChar('#');
        builder->AppendString(*dataCenter);
    }
}

TString ToString(const TNodeDescriptor& descriptor)
{
    return ToStringViaBuilder(descriptor);
}

const TString& GetDefaultAddress(const TAddressMap& addresses)
{
    if (addresses.empty()) {
        return NullNodeAddress();
    }
    return GetOrCrash(addresses, DefaultNetworkName);
}

const TString& GetDefaultAddress(const NProto::TAddressMap& addresses)
{
    if (addresses.entries_size() == 0) {
        return NullNodeAddress();
    }
    for (const auto& entry : addresses.entries()) {
        if (entry.network() == DefaultNetworkName) {
            return entry.address();
        }
    }
    YT_ABORT();
}

EAddressLocality ComputeAddressLocality(const TNodeDescriptor& first, const TNodeDescriptor& second)
{
    if (first.IsNull() || second.IsNull()) {
        return EAddressLocality::None;
    };

    try {
        if (GetServiceHostName(first.GetDefaultAddress()) == GetServiceHostName(second.GetDefaultAddress())) {
            return EAddressLocality::SameHost;
        }

        if (first.GetRack() && second.GetRack() && *first.GetRack() == *second.GetRack()) {
            return EAddressLocality::SameRack;
        }

        if (first.GetDataCenter() && second.GetDataCenter() && *first.GetDataCenter() == *second.GetDataCenter()) {
            return EAddressLocality::SameDataCenter;
        }
    } catch (const std::exception&) {
        // If one of the descriptors is malformed, treat it as None locality and ignore errors.
    }

    return EAddressLocality::None;
}

namespace NProto {

void ToProto(NNodeTrackerClient::NProto::TAddressMap* protoAddresses, const NNodeTrackerClient::TAddressMap& addresses)
{
    for (const auto& [networkName, networkAddress] : addresses) {
        auto* entry = protoAddresses->add_entries();
        entry->set_network(networkName);
        entry->set_address(networkAddress);
    }
}

void FromProto(NNodeTrackerClient::TAddressMap* addresses, const NNodeTrackerClient::NProto::TAddressMap& protoAddresses)
{
    addresses->clear();
    addresses->reserve(protoAddresses.entries_size());
    for (const auto& entry : protoAddresses.entries()) {
        YT_VERIFY(addresses->emplace(entry.network(), entry.address()).second);
    }
}

void ToProto(NNodeTrackerClient::NProto::TNodeAddressMap* proto, const NNodeTrackerClient::TNodeAddressMap& nodeAddresses)
{
    for (const auto& [addressType, addresses] : nodeAddresses) {
        auto* entry = proto->add_entries();
        entry->set_address_type(static_cast<int>(addressType));
        ToProto(entry->mutable_addresses(), addresses);
    }
}

void FromProto(NNodeTrackerClient::TNodeAddressMap* nodeAddresses, const NNodeTrackerClient::NProto::TNodeAddressMap& proto)
{
    nodeAddresses->clear();
    nodeAddresses->reserve(proto.entries_size());
    for (const auto& entry : proto.entries()) {
        NNodeTrackerClient::TAddressMap addresses;
        FromProto(&addresses, entry.addresses());

        YT_VERIFY(nodeAddresses->emplace(static_cast<EAddressType>(entry.address_type()), std::move(addresses)).second);
    }
}

void ToProto(NNodeTrackerClient::NProto::TNodeDescriptor* protoDescriptor, const NNodeTrackerClient::TNodeDescriptor& descriptor)
{
    using NYT::ToProto;

    ToProto(protoDescriptor->mutable_addresses(), descriptor.Addresses());

    if (descriptor.GetRack()) {
        protoDescriptor->set_rack(*descriptor.GetRack());
    } else {
        protoDescriptor->clear_rack();
    }

    if (descriptor.GetDataCenter()) {
        protoDescriptor->set_data_center(*descriptor.GetDataCenter());
    } else {
        protoDescriptor->clear_data_center();
    }

    ToProto(protoDescriptor->mutable_tags(), descriptor.GetTags());
}

void FromProto(NNodeTrackerClient::TNodeDescriptor* descriptor, const NNodeTrackerClient::NProto::TNodeDescriptor& protoDescriptor)
{
    using NYT::FromProto;

    *descriptor = NNodeTrackerClient::TNodeDescriptor(
        FromProto<NNodeTrackerClient::TAddressMap>(protoDescriptor.addresses()),
        protoDescriptor.has_rack() ? std::make_optional(protoDescriptor.rack()) : std::nullopt,
        protoDescriptor.has_data_center() ? std::make_optional(protoDescriptor.data_center()) : std::nullopt,
        FromProto<std::vector<TString>>(protoDescriptor.tags()));
}

} // namespace NProto

bool operator == (const TNodeDescriptor& lhs, const TNodeDescriptor& rhs)
{
    return
        lhs.GetDefaultAddress() == rhs.GetDefaultAddress() && // shortcut
        lhs.Addresses() == rhs.Addresses() &&
        lhs.GetRack() == rhs.GetRack() &&
        lhs.GetDataCenter() == rhs.GetDataCenter() &&
        GetSortedTags(lhs.GetTags()) == GetSortedTags(rhs.GetTags());
}

bool operator != (const TNodeDescriptor& lhs, const TNodeDescriptor& rhs)
{
    return !(lhs == rhs);
}

bool operator == (const TNodeDescriptor& lhs, const NProto::TNodeDescriptor& rhs)
{
    if (lhs.Addresses().size() != rhs.addresses().entries_size()) {
        return false;
    }

    for (const auto& protoEntry : rhs.addresses().entries()) {
        const auto& network = protoEntry.network();
        const auto& address = protoEntry.address();
        auto it = lhs.Addresses().find(network);
        if (it == lhs.Addresses().end()) {
            return false;
        }
        if (it->second != address) {
            return false;
        }
    }

    const auto& lhsMaybeRack = lhs.GetRack();
    auto lhsRack = lhsMaybeRack ? TStringBuf(*lhsMaybeRack) : TStringBuf();
    if (lhsRack != rhs.rack()) {
        return false;
    }

    const auto& lhsMaybeDataCenter = lhs.GetDataCenter();
    auto lhsDataCenter = lhsMaybeDataCenter ? TStringBuf(*lhsMaybeDataCenter) : TStringBuf();
    if (lhsDataCenter != rhs.data_center()) {
        return false;
    }

    const auto& lhsTags = lhs.GetTags();
    auto rhsTags = FromProto<std::vector<TString>>(rhs.tags());
    if (GetSortedTags(lhsTags) != GetSortedTags(rhsTags)) {
        return false;
    }

    return true;
}

bool operator != (const TNodeDescriptor& lhs, const NProto::TNodeDescriptor& rhs)
{
    return !(lhs == rhs);
}

////////////////////////////////////////////////////////////////////////////////

void TNodeDirectory::MergeFrom(const NProto::TNodeDirectory& source)
{
    std::vector<const NProto::TNodeDirectory_TItem*> items;
    items.reserve(source.items_size());
    {
        auto guard = ReaderGuard(SpinLock_);
        for (const auto& item : source.items()) {
            if (NeedToAddDescriptor(item.node_id(), item.node_descriptor())) {
                items.push_back(&item);
            }
        }
    }
    {
        auto guard = WriterGuard(SpinLock_);
        for (const auto* item : items) {
            DoAddDescriptor(item->node_id(), item->node_descriptor());
        }
    }
}

void TNodeDirectory::MergeFrom(const TNodeDirectoryPtr& source)
{
    if (this == source.Get()) {
        return;
    }

    std::vector<std::pair<TNodeId, TNodeDescriptor>> items;
    {
        auto thisGuard = WriterGuard(SpinLock_);
        auto sourceGuard = ReaderGuard(source->SpinLock_);
        items.reserve(source->IdToDescriptor_.size());
        for (auto [id, descriptor] : source->IdToDescriptor_) {
            if (NeedToAddDescriptor(id, *descriptor)) {
                items.emplace_back(id, *descriptor);
            }
        }
    }
    {
        auto thisGuard = WriterGuard(SpinLock_);
        for (const auto& [id, descriptor] : items) {
            DoAddDescriptor(id, descriptor);
        }
    }
}

void TNodeDirectory::DumpTo(NProto::TNodeDirectory* destination)
{
    auto guard = ReaderGuard(SpinLock_);
    for (auto [id, descriptor] : IdToDescriptor_) {
        auto* item = destination->add_items();
        item->set_node_id(id);
        ToProto(item->mutable_node_descriptor(), *descriptor);
    }
}

void TNodeDirectory::Serialize(IYsonConsumer* consumer) const
{
    auto guard = ReaderGuard(SpinLock_);

    BuildYsonFluently(consumer)
        .BeginList()
            .DoFor(IdToDescriptor_, [&] (TFluentList fluent, const std::pair<TNodeId, const TNodeDescriptor*>& pair) {
                fluent
                    .Item()
                        .BeginMap()
                            .Item("node_id").Value(pair.first)
                            .Item("addresses").Value(pair.second->Addresses())
                        .EndMap();
            })
        .EndList();
}

void Serialize(const TNodeDirectory& nodeDirectory, NYson::IYsonConsumer* consumer)
{
    nodeDirectory.Serialize(consumer);
}

void TNodeDirectory::AddDescriptor(TNodeId id, const TNodeDescriptor& descriptor)
{
    auto guard = WriterGuard(SpinLock_);
    DoAddDescriptor(id, descriptor);
}

bool TNodeDirectory::NeedToAddDescriptor(TNodeId id, const TNodeDescriptor& descriptor)
{
    auto it = IdToDescriptor_.find(id);
    return it == IdToDescriptor_.end() || *it->second != descriptor;
}

void TNodeDirectory::DoAddDescriptor(TNodeId id, const TNodeDescriptor& descriptor)
{
    if (!NeedToAddDescriptor(id, descriptor)) {
        return;
    }
    DoCaptureAndAddDescriptor(id, TNodeDescriptor(descriptor));
}

bool TNodeDirectory::NeedToAddDescriptor(TNodeId id, const NProto::TNodeDescriptor& descriptor)
{
    auto it = IdToDescriptor_.find(id);
    return it == IdToDescriptor_.end() || *it->second != descriptor;
}

void TNodeDirectory::DoAddDescriptor(TNodeId id, const NProto::TNodeDescriptor& protoDescriptor)
{
    if (!NeedToAddDescriptor(id, protoDescriptor)) {
        return;
    }
    DoCaptureAndAddDescriptor(id, FromProto<TNodeDescriptor>(protoDescriptor));
}

void TNodeDirectory::DoCaptureAndAddDescriptor(TNodeId id, TNodeDescriptor&& descriptor)
{
    auto it = Descriptors_.find(descriptor);
    if (it == Descriptors_.end()) {
        it = Descriptors_.insert(std::move(descriptor)).first;
    }
    const auto* capturedDescriptor = &*it;
    IdToDescriptor_[id] = capturedDescriptor;
    AddressToDescriptor_[capturedDescriptor->GetDefaultAddress()] = capturedDescriptor;
}

const TNodeDescriptor* TNodeDirectory::FindDescriptor(TNodeId id) const
{
    auto guard = ReaderGuard(SpinLock_);
    auto it = IdToDescriptor_.find(id);
    return it == IdToDescriptor_.end() ? nullptr : it->second;
}

const TNodeDescriptor& TNodeDirectory::GetDescriptor(TNodeId id) const
{
    const auto* result = FindDescriptor(id);
    YT_VERIFY(result);
    return *result;
}

const TNodeDescriptor& TNodeDirectory::GetDescriptor(TChunkReplica replica) const
{
    return GetDescriptor(replica.GetNodeId());
}

std::vector<TNodeDescriptor> TNodeDirectory::GetDescriptors(const TChunkReplicaList& replicas) const
{
    std::vector<TNodeDescriptor> result;
    for (auto replica : replicas) {
        result.push_back(GetDescriptor(replica));
    }
    return result;
}

std::vector<std::pair<TNodeId, TNodeDescriptor>> TNodeDirectory::GetAllDescriptors() const
{
    auto guard = ReaderGuard(SpinLock_);

    std::vector<std::pair<TNodeId, TNodeDescriptor>> result;
    result.reserve(IdToDescriptor_.size());
    for (auto [id, descriptor] : IdToDescriptor_) {
        result.emplace_back(id, *descriptor);
    }
    return result;
}

const TNodeDescriptor* TNodeDirectory::FindDescriptor(const TString& address)
{
    auto guard = ReaderGuard(SpinLock_);
    auto it = AddressToDescriptor_.find(address);
    return it == AddressToDescriptor_.end() ? nullptr : it->second;
}

const TNodeDescriptor& TNodeDirectory::GetDescriptor(const TString& address)
{
    const auto* result = FindDescriptor(address);
    YT_VERIFY(result);
    return *result;
}

void TNodeDirectory::Save(TStreamSaveContext& context) const
{
    THashMap<TNodeId, TNodeDescriptor> idToDescriptor;
    {
        auto guard = ReaderGuard(SpinLock_);
        for (auto [id, descriptor] : IdToDescriptor_) {
            YT_VERIFY(idToDescriptor.emplace(id, *descriptor).second);
        }
    }
    using NYT::Save;
    Save(context, idToDescriptor);
}

void TNodeDirectory::Load(TStreamLoadContext& context)
{
    using NYT::Load;
    auto idToDescriptor = Load<THashMap<TNodeId, TNodeDescriptor>>(context);
    auto guard = WriterGuard(SpinLock_);
    for (const auto& [id, descriptor] : idToDescriptor) {
        DoAddDescriptor(id, descriptor);
    }
}

////////////////////////////////////////////////////////////////////////////////

namespace {

TAddressMap::const_iterator SelectAddress(const TAddressMap& addresses, const TNetworkPreferenceList& networks)
{
    for (const auto& network : networks) {
        const auto it = addresses.find(network);
        if (it != addresses.cend()) {
            return it;
        }
    }

    return addresses.cend();
}

} // namespace

std::optional<TString> FindAddress(const TAddressMap& addresses, const TNetworkPreferenceList& networks)
{
    const auto it = SelectAddress(addresses, networks);
    return it == addresses.cend() ? std::nullopt : std::make_optional(it->second);
}

const TString& GetAddressOrThrow(const TAddressMap& addresses, const TNetworkPreferenceList& networks)
{
    const auto it = SelectAddress(addresses, networks);
    if (it != addresses.cend()) {
        return it->second;
    }

    THROW_ERROR_EXCEPTION("Cannot select address for host %v since there is no compatible network",
            GetDefaultAddress(addresses))
            << TErrorAttribute("remote_networks", GetKeys(addresses))
            << TErrorAttribute("local_networks", networks);
}

TAddressWithNetwork GetAddressWithNetworkOrThrow(const TAddressMap& addresses, const TNetworkPreferenceList& networks)
{
    const auto it = SelectAddress(addresses, networks);
    if (it != addresses.cend()) {
        return TAddressWithNetwork{it->second, it->first};
    }

    THROW_ERROR_EXCEPTION("Cannot select address for host %v since there is no compatible network",
        GetDefaultAddress(addresses))
        << TErrorAttribute("remote_networks", GetKeys(addresses))
        << TErrorAttribute("local_networks", networks);
}

const TAddressMap& GetAddressesOrThrow(const TNodeAddressMap& nodeAddresses, EAddressType type)
{
    auto it = nodeAddresses.find(type);
    if (it != nodeAddresses.cend()) {
        return it->second;
    }

    THROW_ERROR_EXCEPTION("No addresses known for address type %Qlv", type)
        << TErrorAttribute("known_types", GetKeys(nodeAddresses));
}

////////////////////////////////////////////////////////////////////////////////

} // namespace NYT::NNodeTrackerClient

////////////////////////////////////////////////////////////////////////////////

size_t THash<NYT::NNodeTrackerClient::TNodeDescriptor>::operator()(
    const NYT::NNodeTrackerClient::TNodeDescriptor& nodeDescriptor) const
{
    size_t result = 0;
    using namespace NYT;
    HashCombine(result, nodeDescriptor.GetDefaultAddress());
    HashCombine(result, nodeDescriptor.GetRack());
    HashCombine(result, nodeDescriptor.GetDataCenter());
    for (const auto& [network, address] : nodeDescriptor.Addresses()) {
        HashCombine(result, network);
        HashCombine(result, address);
    }
    for (const auto& tag : NYT::NNodeTrackerClient::GetSortedTags(nodeDescriptor.GetTags())) {
        HashCombine(result, tag);
    }
    return result;
}

////////////////////////////////////////////////////////////////////////////////
