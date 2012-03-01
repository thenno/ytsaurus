#ifndef LOAD_CONTEXT_INL_H_
#error "Direct inclusion of this file is not allowed, include load_context.h"
#endif
#undef LOAD_CONTEXT_INL_H_

#include <ytlib/misc/foreach.h>

//#include <util/ysaveload.h>

namespace NYT {
namespace NCellMaster {

////////////////////////////////////////////////////////////////////////////////

template <class T>
void LoadObject(TInputStream* input, T*& object, const TLoadContext& context)
{
    NObjectServer::TObjectId objectId;
    ::Load(input, objectId);
    if (objectId == NObjectServer::NullObjectId) {
        object = NULL;
    } else {
        object = context.Get<T>(objectId);
    }
}

template <class T>
void SaveObjects(TOutputStream* output, const T& objects)
{
    ::SaveSize(output, objects.size());
    FOREACH (auto* object, objects) {
        ::Save(output, object->GetId());
    }
}


template <class T>
void LoadObjects(TInputStream* input, std::vector<T*>& objects, const TLoadContext& context)
{
    auto size = ::LoadSize(input);
    objects.reserve(size);
    for (size_t i = 0; i < size; ++i) {
        T* object;
        LoadObject(input, object, context);
        objects.push_back(object);
    }
}

template <class T>
void LoadObjects(TInputStream* input, yhash_set<T*>& objects, const TLoadContext& context)
{
    auto size = ::LoadSize(input);
    // objects.resize(size); // Do we need this?
    for (size_t i = 0; i < size; ++i) {
        T* object;
        LoadObject(input, object, context);
        YVERIFY(objects.insert(object).second);
    }
}

////////////////////////////////////////////////////////////////////////////////
            
} // namespace NCellMaster
} // namespace NYT
