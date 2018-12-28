#pragma once

#include "public.h"

#include <yt/server/cell_master/public.h>

#include <yt/server/object_server/public.h>

#include <yt/core/misc/property.h>
#include <yt/core/misc/small_vector.h>

#include <yt/core/yson/public.h>

#include <yt/core/ytree/permission.h>
#include <yt/core/ytree/yson_serializable.h>

namespace NYT::NSecurityServer {

////////////////////////////////////////////////////////////////////////////////

const int TypicalSubjectCount = 4;
typedef SmallVector<TSubject*, TypicalSubjectCount> TSubjectList;

struct TAccessControlEntry
{
    TAccessControlEntry();
    TAccessControlEntry(
        ESecurityAction action,
        TSubject* subject,
        EPermissionSet permissions,
        EAceInheritanceMode inheritanceMode = EAceInheritanceMode::ObjectAndDescendants);

    ESecurityAction Action;
    TSubjectList Subjects;
    EPermissionSet Permissions;
    EAceInheritanceMode InheritanceMode;

    void Persist(NCellMaster::TPersistenceContext& context);
};

void Serialize(const TAccessControlEntry& ace, NYson::IYsonConsumer* consumer);

////////////////////////////////////////////////////////////////////////////////

struct TAccessControlList
{
    std::vector<TAccessControlEntry> Entries;
};

void Load(NCellMaster::TLoadContext& context, TAccessControlList& acl);
void Save(NCellMaster::TSaveContext& context, const TAccessControlList& acl);

void Serialize(const TAccessControlList& acl, NYson::IYsonConsumer* consumer);
void Deserialize(
    TAccessControlList& acl,
    NYTree::INodePtr node,
    TSecurityManagerPtr securityManager,
    // Puts missing subjects in this array. Throws an error on missing subjects if nullptr.
    std::vector<TString>* missingSubjects = nullptr);

////////////////////////////////////////////////////////////////////////////////

class TAccessControlDescriptor
{
    DEFINE_BYREF_RO_PROPERTY(TAccessControlList, Acl);
    DEFINE_BYVAL_RW_PROPERTY(bool, Inherit, true);
    DEFINE_BYVAL_RO_PROPERTY(NObjectServer::TObjectBase*, Object);

public:
    explicit TAccessControlDescriptor(NObjectServer::TObjectBase* object = nullptr);

    void Clear();

    TSubject* GetOwner() const;
    void SetOwner(TSubject* owner);

    void AddEntry(const TAccessControlEntry& ace);
    void ClearEntries();

    void OnSubjectDestroyed(TSubject* subject, TSubject* defaultOwner);

    void Save(NCellMaster::TSaveContext& context) const;
    void Load(NCellMaster::TLoadContext& context);

private:
    TSubject* Owner_ = nullptr;

};

////////////////////////////////////////////////////////////////////////////////

struct TSerializableAccessControlEntry
    : public NYTree::TYsonSerializable
{
    ESecurityAction Action;
    std::vector<TString> Subjects;
    std::vector<TString> Permissions;
    EAceInheritanceMode InheritanceMode;

    TSerializableAccessControlEntry();
};

DEFINE_REFCOUNTED_TYPE(TSerializableAccessControlEntry);

////////////////////////////////////////////////////////////////////////////////


} // namespace NYT::NSecurityServer
