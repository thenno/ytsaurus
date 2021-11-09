#ifndef ASYNC_SLRU_CACHE_INL_H_
#error "Direct inclusion of this file is not allowed, include async_slru_cache.h"
// For the sake of sane code completion.
#include "async_slru_cache.h"
#endif

#include <util/system/yield.h>

namespace NYT {

////////////////////////////////////////////////////////////////////////////////

template <class TKey, class TValue, class THash>
TAsyncSlruCacheBase<TKey, TValue, THash>::TItem::TItem()
    : ValuePromise(NewPromise<TValuePtr>())
{ }

template <class TKey, class TValue, class THash>
TAsyncSlruCacheBase<TKey, TValue, THash>::TItem::TItem(TValuePtr value)
    : ValuePromise(MakePromise(value))
    , Value(std::move(value))
{ }

template <class TKey, class TValue, class THash>
auto TAsyncSlruCacheBase<TKey, TValue, THash>::TItem::GetValueFuture() const -> TValueFuture
{
    return ValuePromise
        .ToFuture()
        .ToUncancelable();
}

////////////////////////////////////////////////////////////////////////////////

template <class TKey, class TValue, class THash>
const TKey& TAsyncCacheValueBase<TKey, TValue, THash>::GetKey() const
{
    return Key_;
}

template <class TKey, class TValue, class THash>
void TAsyncCacheValueBase<TKey, TValue, THash>::UpdateWeight() const
{
    if (auto cache = Cache_.Lock()) {
        cache->UpdateWeight(GetKey());
    }
}

template <class TKey, class TValue, class THash>
TAsyncCacheValueBase<TKey, TValue, THash>::TAsyncCacheValueBase(const TKey& key)
    : Key_(key)
{ }

template <class TKey, class TValue, class THash>
NYT::TAsyncCacheValueBase<TKey, TValue, THash>::~TAsyncCacheValueBase()
{
    if (auto cache = Cache_.Lock()) {
        cache->Unregister(Key_);
    }
}

////////////////////////////////////////////////////////////////////////////////

template <class TKey, class TValue, class THash>
TAsyncSlruCacheBase<TKey, TValue, THash>::TAsyncSlruCacheBase(
    TSlruCacheConfigPtr config,
    const NProfiling::TProfiler& profiler)
    : Config_(std::move(config))
    , Capacity_(Config_->Capacity)
    , YoungerSizeFraction_(Config_->YoungerSizeFraction)
    , SyncHitWeightCounter_(profiler.Counter("/hit_weight_sync"))
    , AsyncHitWeightCounter_(profiler.Counter("/hit_weight_async"))
    , MissedWeightCounter_(profiler.Counter("/missed_weight"))
    , SyncHitCounter_(profiler.Counter("/hit_count_sync"))
    , AsyncHitCounter_(profiler.Counter("/hit_count_async"))
    , MissedCounter_(profiler.Counter("/missed_count"))
{
    static_assert(
        std::is_base_of_v<TAsyncCacheValueBase<TKey, TValue, THash>, TValue>,
        "TValue must be derived from TAsyncCacheValueBase");

    profiler.AddFuncGauge("/younger_weight", MakeStrong(this), [this] {
        return YoungerWeightCounter_.load();
    });
    profiler.AddFuncGauge("/older_weight", MakeStrong(this), [this] {
        return OlderWeightCounter_.load();
    });
    profiler.AddFuncGauge("/younger_size", MakeStrong(this), [this] {
        return YoungerSizeCounter_.load();
    });
    profiler.AddFuncGauge("/older_size", MakeStrong(this), [this] {
        return OlderSizeCounter_.load();
    });

    YT_VERIFY(IsPowerOf2(Config_->ShardCount));
    Shards_.reset(new TShard[Config_->ShardCount]);

    int touchBufferCapacity = Config_->TouchBufferCapacity / Config_->ShardCount;
    for (int index = 0; index < Config_->ShardCount; ++index) {
        Shards_[index].TouchBuffer.resize(touchBufferCapacity);
    }
}

template <class TKey, class TValue, class THash>
void TAsyncSlruCacheBase<TKey, TValue, THash>::Clear()
{
    for (int shardIndex = 0; shardIndex < Config_->ShardCount; ++shardIndex) {
        auto* shard = Shards_[shardIndex];

        auto writerGuard = WriterGuard(shard->SpinLock);

        shard->TouchBufferPosition = 0;

        Size_ -= std::ssize(shard->ItemMap);
        shard->ItemMap.clear();

        TIntrusiveListWithAutoDelete<TItem, TDelete> youngerLruList;
        shard->YoungerLruList.Swap(youngerLruList);

        TIntrusiveListWithAutoDelete<TItem, TDelete> olderLruList;
        shard->OlderLruList.Swap(olderLruList);

        i64 totalYoungerWeight = 0;
        i64 totalOlderWeight = 0;
        for (const auto& item : youngerLruList) {
            totalYoungerWeight += GetWeight(item.Value);
        }
        for (const auto& item : olderLruList) {
            totalOlderWeight += GetWeight(item.Value);
        }

        shard->YoungerWeightCounter -= totalYoungerWeight;
        shard->OlderWeightCounter -= totalOlderWeight;
        YoungerWeightCounter_.fetch_sub(totalYoungerWeight);
        OlderWeightCounter_.fetch_sub(totalOlderWeight);
        YoungerSizeCounter_.fetch_sub(std::ssize(youngerLruList));
        OlderSizeCounter_.fetch_sub(std::ssize(olderLruList));

        // NB: Lists must die outside the critical section.
        writerGuard.Release();
    }
}

template <class TKey, class TValue, class THash>
void TAsyncSlruCacheBase<TKey, TValue, THash>::Reconfigure(const TSlruCacheDynamicConfigPtr& config)
{
    Capacity_.store(config->Capacity.value_or(Config_->Capacity));
    YoungerSizeFraction_.store(config->YoungerSizeFraction.value_or(Config_->YoungerSizeFraction));

    for (int shardIndex = 0; shardIndex < Config_->ShardCount; ++shardIndex) {
        auto& shard = Shards_[shardIndex];

        auto writerGuard = WriterGuard(shard.SpinLock);

        DrainTouchBuffer(&shard);
        Trim(&shard, writerGuard);
    }
}

template <class TKey, class TValue, class THash>
typename TAsyncSlruCacheBase<TKey, TValue, THash>::TValuePtr
TAsyncSlruCacheBase<TKey, TValue, THash>::Find(const TKey& key)
{
    auto* shard = GetShardByKey(key);

    auto readerGuard = ReaderGuard(shard->SpinLock);

    auto itemIt = shard->ItemMap.find(key);
    if (itemIt == shard->ItemMap.end()) {
        MissedCounter_.Increment();
        return nullptr;
    }

    auto* item = itemIt->second;
    auto value = item->Value;
    if (!value) {
        MissedCounter_.Increment();
        return nullptr;
    }

    bool needToDrain = Touch(shard, item);

    SyncHitWeightCounter_.Increment(item->CachedWeight);
    SyncHitCounter_.Increment();

    readerGuard.Release();

    if (needToDrain) {
        auto writerGuard = WriterGuard(shard->SpinLock);
        DrainTouchBuffer(shard);
    }

    return value;
}

template <class TKey, class TValue, class THash>
int TAsyncSlruCacheBase<TKey, TValue, THash>::GetSize() const
{
    return Size_.load();
}

template <class TKey, class TValue, class THash>
i64 TAsyncSlruCacheBase<TKey, TValue, THash>::GetCapacity() const
{
    return Capacity_.load();
}

template <class TKey, class TValue, class THash>
std::vector<typename TAsyncSlruCacheBase<TKey, TValue, THash>::TValuePtr>
TAsyncSlruCacheBase<TKey, TValue, THash>::GetAll()
{
    std::vector<TValuePtr> result;
    result.reserve(GetSize());

    for (int shardIndex = 0; shardIndex < Config_->ShardCount; ++shardIndex) {
        const auto& shard = Shards_[shardIndex];

        auto readerGuard = ReaderGuard(shard.SpinLock);
        for (const auto& [key, rawValue] : shard.ValueMap) {
            if (auto value = DangerousGetPtr<TValue>(rawValue)) {
                result.push_back(value);
            }
        }
    }
    return result;
}

template <class TKey, class TValue, class THash>
typename TAsyncSlruCacheBase<TKey, TValue, THash>::TValueFuture
TAsyncSlruCacheBase<TKey, TValue, THash>::Lookup(const TKey& key)
{
    auto* shard = GetShardByKey(key);
    auto valueFuture = DoLookup(shard, key);
    if (!valueFuture) {
        MissedCounter_.Increment();
    }
    return valueFuture;
}

template <class TKey, class TValue, class THash>
void TAsyncSlruCacheBase<TKey, TValue, THash>::Touch(const TValuePtr& value)
{
    auto* shard = GetShardByKey(value->GetKey());
    auto readerGuard = ReaderGuard(shard->SpinLock);

    if (!value->Item_) {
        return;
    }

    auto needToDrain = Touch(shard, value->Item_);

    readerGuard.Release();

    if (needToDrain) {
        auto writerGuard = WriterGuard(shard->SpinLock);
        DrainTouchBuffer(shard);
    }
}

template <class TKey, class TValue, class THash>
typename TAsyncSlruCacheBase<TKey, TValue, THash>::TValueFuture
TAsyncSlruCacheBase<TKey, TValue, THash>::DoLookup(TShard* shard, const TKey& key)
{
    auto readerGuard = ReaderGuard(shard->SpinLock);

    auto& itemMap = shard->ItemMap;
    const auto& valueMap = shard->ValueMap;

    if (auto itemIt = itemMap.find(key); itemIt != itemMap.end()) {
        auto* item = itemIt->second;
        bool needToDrain = Touch(shard, item);
        auto valueFuture = item->GetValueFuture();

        if (item->Value) {
            SyncHitWeightCounter_.Increment(item->CachedWeight);
            SyncHitCounter_.Increment();
        } else {
            AsyncHitCounter_.Increment();
            item->AsyncHitCount.fetch_add(1);
        }

        readerGuard.Release();

        if (needToDrain) {
            auto writerGuard = WriterGuard(shard->SpinLock);
            DrainTouchBuffer(shard);
        }

        return valueFuture;
    }

    auto valueIt = valueMap.find(key);
    if (valueIt == valueMap.end()) {
        return {};
    }

    auto value = DangerousGetPtr(valueIt->second);
    if (!value) {
        return {};
    }

    readerGuard.Release();

    auto writerGuard = WriterGuard(shard->SpinLock);

    if (auto itemIt = itemMap.find(key); itemIt != itemMap.end()) {
        auto* item = itemIt->second;

        Touch(shard, item);
        auto valueFuture = item->GetValueFuture();

        if (item->Value) {
            SyncHitWeightCounter_.Increment(item->CachedWeight);
            SyncHitCounter_.Increment();
        } else {
            AsyncHitCounter_.Increment();
            item->AsyncHitCount.fetch_add(1);
        }

        DrainTouchBuffer(shard);

        return valueFuture;
    }

    DrainTouchBuffer(shard);

    {
        auto* item = new TItem(value);
        value->Item_ = item;

        auto valueFuture = item->GetValueFuture();

        YT_VERIFY(itemMap.emplace(key, item).second);
        ++Size_;

        i64 weight = PushToYounger(shard, item);
        SyncHitWeightCounter_.Increment(weight);
        SyncHitCounter_.Increment();

        // NB: Releases the lock.
        FinishInsertAndTrim(shard, writerGuard, value);

        return valueFuture;
    }
}

template <class TKey, class TValue, class THash>
auto TAsyncSlruCacheBase<TKey, TValue, THash>::BeginInsert(const TKey& key) -> TInsertCookie
{
    auto* shard = GetShardByKey(key);

    if (auto valueFuture = DoLookup(shard, key)) {
        return TInsertCookie(
            key,
            nullptr,
            std::move(valueFuture),
            false);
    }

    while (true) {
        auto guard = WriterGuard(shard->SpinLock);

        DrainTouchBuffer(shard);

        auto& itemMap = shard->ItemMap;
        auto& valueMap = shard->ValueMap;

        auto itemIt = itemMap.find(key);
        if (itemIt != itemMap.end()) {
            auto* item = itemIt->second;
            Touch(shard, item);
            auto valueFuture = item->GetValueFuture();

            if (item->Value) {
                SyncHitWeightCounter_.Increment(item->CachedWeight);
                SyncHitCounter_.Increment();
            } else {
                AsyncHitCounter_.Increment();
                item->AsyncHitCount.fetch_add(1);
            }

            return TInsertCookie(
                key,
                nullptr,
                std::move(valueFuture),
                false);
        }

        auto valueIt = valueMap.find(key);
        if (valueIt == valueMap.end()) {
            auto* item = new TItem();
            auto valueFuture = item->GetValueFuture();

            YT_VERIFY(itemMap.emplace(key, item).second);
            ++Size_;

            MissedCounter_.Increment();

            return TInsertCookie(
                key,
                this,
                std::move(valueFuture),
                true);
        }

        if (auto value = DangerousGetPtr(valueIt->second)) {
            auto* item = new TItem(value);
            value->Item_ = item;

            YT_VERIFY(itemMap.emplace(key, item).second);
            ++Size_;

            i64 weight = PushToYounger(shard, item);
            SyncHitWeightCounter_.Increment(weight);
            SyncHitCounter_.Increment();

            // NB: Releases the lock.
            FinishInsertAndTrim(shard, guard, value);

            return TInsertCookie(
                key,
                nullptr,
                MakeFuture(value),
                false);
        }

        // Back off.
        // Hopefully the object we had just extracted will be destroyed soon
        // and thus vanish from ValueMap.
        guard.Release();
        ThreadYield();
    }
}

template <class TKey, class TValue, class THash>
void TAsyncSlruCacheBase<TKey, TValue, THash>::EndInsert(TValuePtr value)
{
    YT_VERIFY(value);
    auto key = value->GetKey();

    auto* shard = GetShardByKey(key);

    auto guard = WriterGuard(shard->SpinLock);

    DrainTouchBuffer(shard);

    value->Cache_ = MakeWeak(this);

    auto* item = GetOrCrash(shard->ItemMap, key);
    item->Value = value;
    value->Item_ = item;
    auto promise = item->ValuePromise;

    YT_VERIFY(shard->ValueMap.emplace(key, value.Get()).second);

    i64 weight = PushToYounger(shard, item);
    // MissedCounter_ and AsyncHitCounter_ have already been incremented in BeginInsert.
    MissedWeightCounter_.Increment(weight);
    AsyncHitWeightCounter_.Increment(weight * item->AsyncHitCount.load());

    // NB: Releases the lock.
    FinishInsertAndTrim(shard, guard, value);

    promise.Set(value);
}

template <class TKey, class TValue, class THash>
void TAsyncSlruCacheBase<TKey, TValue, THash>::CancelInsert(const TKey& key, const TError& error)
{
    auto* shard = GetShardByKey(key);

    auto guard = WriterGuard(shard->SpinLock);

    DrainTouchBuffer(shard);

    auto& itemMap = shard->ItemMap;
    auto itemIt = itemMap.find(key);
    YT_VERIFY(itemIt != itemMap.end());

    auto* item = itemIt->second;
    auto promise = item->ValuePromise;

    itemMap.erase(itemIt);
    --Size_;

    YT_VERIFY(!item->Value);

    delete item;

    guard.Release();

    promise.Set(error);
}

template <class TKey, class TValue, class THash>
void TAsyncSlruCacheBase<TKey, TValue, THash>::Unregister(const TKey& key)
{
    auto* shard = GetShardByKey(key);

    auto guard = WriterGuard(shard->SpinLock);

    DrainTouchBuffer(shard);

    YT_VERIFY(shard->ItemMap.find(key) == shard->ItemMap.end());
    YT_VERIFY(shard->ValueMap.erase(key) == 1);
}

template <class TKey, class TValue, class THash>
void TAsyncSlruCacheBase<TKey, TValue, THash>::TryRemove(const TKey& key, bool forbidResurrection)
{
    DoTryRemove(key, nullptr, forbidResurrection);
}

template <class TKey, class TValue, class THash>
void TAsyncSlruCacheBase<TKey, TValue, THash>::TryRemoveValue(const TValuePtr& value, bool forbidResurrection)
{
    DoTryRemove(value->GetKey(), value, forbidResurrection);
}

template <class TKey, class TValue, class THash>
void TAsyncSlruCacheBase<TKey, TValue, THash>::DoTryRemove(
    const TKey& key,
    const TValuePtr& value,
    bool forbidResurrection)
{
    auto* shard = GetShardByKey(key);

    auto guard = WriterGuard(shard->SpinLock);

    DrainTouchBuffer(shard);

    auto& itemMap = shard->ItemMap;
    auto& valueMap = shard->ValueMap;

    auto valueIt = valueMap.find(key);
    if (valueIt == valueMap.end()) {
        return;
    }

    if (value && valueIt->second != value) {
        return;
    }

    if (forbidResurrection || !IsResurrectionSupported()) {
        valueIt->second->Cache_.Reset();
        valueMap.erase(valueIt);
    }

    auto itemIt = itemMap.find(key);
    if (itemIt == itemMap.end()) {
        return;
    }

    auto* item = itemIt->second;
    auto actualValue = item->Value;
    if (!actualValue) {
        return;
    }

    itemMap.erase(itemIt);
    --Size_;

    Pop(shard, item);

    if (value) {
        YT_VERIFY(value->Item_ == item);
        value->Item_ = nullptr;
    }

    delete item;

    guard.Release();

    OnRemoved(actualValue);
}

template <class TKey, class TValue, class THash>
void TAsyncSlruCacheBase<TKey, TValue, THash>::UpdateWeight(const TKey& key)
{
    auto* shard = GetShardByKey(key);

    auto guard = WriterGuard(shard->SpinLock);

    DrainTouchBuffer(shard);

    auto itemIt = shard->ItemMap.find(key);
    if (itemIt == shard->ItemMap.end()) {
        return;
    }

    auto item = itemIt->second;
    if (!item->Value) {
        return;
    }

    i64 newWeight = GetWeight(item->Value);
    i64 weightDelta = newWeight - item->CachedWeight;

    YT_VERIFY(!item->Empty());
    if (item->Younger) {
        shard->YoungerWeightCounter += weightDelta;
        YoungerWeightCounter_.fetch_add(weightDelta);
    } else {
        shard->OlderWeightCounter += weightDelta;
        OlderWeightCounter_.fetch_add(weightDelta);
    }

    // If item weight increases, it means that some parts of the item were missing in cache,
    // so add delta to missed weight.
    if (weightDelta > 0) {
        MissedWeightCounter_.Increment(weightDelta);
    }

    item->CachedWeight = newWeight;

    Trim(shard, guard);
}

template <class TKey, class TValue, class THash>
void TAsyncSlruCacheBase<TKey, TValue, THash>::UpdateWeight(const TValuePtr& value)
{
    UpdateWeight(value->GetKey());
}


template <class TKey, class TValue, class THash>
auto TAsyncSlruCacheBase<TKey, TValue, THash>::GetShardByKey(const TKey& key) const -> TShard*
{
    return &Shards_[THash()(key) & (Config_->ShardCount - 1)];
}

template <class TKey, class TValue, class THash>
bool TAsyncSlruCacheBase<TKey, TValue, THash>::Touch(TShard* shard, TItem* item)
{
    if (!item->Value) {
        return false;
    }

    int capacity = std::ssize(shard->TouchBuffer);
    int index = shard->TouchBufferPosition++;
    if (index >= capacity) {
        // Drop touch request due to buffer overflow.
        // NB: We still return false since the other thread is already responsible for
        // draining the buffer.
        return false;
    }

    shard->TouchBuffer[index] = item;
    return index == capacity - 1;
}

template <class TKey, class TValue, class THash>
void TAsyncSlruCacheBase<TKey, TValue, THash>::DrainTouchBuffer(TShard* shard)
{
    int count = std::min<int>(
        shard->TouchBufferPosition.load(),
        std::ssize(shard->TouchBuffer));
    for (int index = 0; index < count; ++index) {
        MoveToOlder(shard, shard->TouchBuffer[index]);
    }
    shard->TouchBufferPosition = 0;
}

template <class TKey, class TValue, class THash>
i64 TAsyncSlruCacheBase<TKey, TValue, THash>::GetWeight(const TValuePtr& /*value*/) const
{
    return 1;
}

template <class TKey, class TValue, class THash>
void TAsyncSlruCacheBase<TKey, TValue, THash>::OnAdded(const TValuePtr& /*value*/)
{ }

template <class TKey, class TValue, class THash>
void TAsyncSlruCacheBase<TKey, TValue, THash>::OnRemoved(const TValuePtr& /*value*/)
{ }

template <class TKey, class TValue, class THash>
bool TAsyncSlruCacheBase<TKey, TValue, THash>::IsResurrectionSupported() const
{
    return true;
}

template <class TKey, class TValue, class THash>
i64 TAsyncSlruCacheBase<TKey, TValue, THash>::PushToYounger(TShard* shard, TItem* item)
{
    YT_ASSERT(item->Empty());
    shard->YoungerLruList.PushFront(item);
    i64 weight = GetWeight(item->Value);
    shard->YoungerWeightCounter += weight;
    item->CachedWeight = weight;
    YoungerWeightCounter_.fetch_add(weight);
    YoungerSizeCounter_.fetch_add(1);
    item->Younger = true;
    return weight;
}

template <class TKey, class TValue, class THash>
void TAsyncSlruCacheBase<TKey, TValue, THash>::MoveToYounger(TShard* shard, TItem* item)
{
    YT_ASSERT(!item->Empty());
    item->Unlink();
    shard->YoungerLruList.PushFront(item);
    if (!item->Younger) {
        i64 weight = item->CachedWeight;
        shard->OlderWeightCounter -= weight;
        OlderWeightCounter_.fetch_sub(weight);
        OlderSizeCounter_.fetch_sub(1);
        shard->YoungerWeightCounter += weight;
        YoungerWeightCounter_.fetch_add(weight);
        YoungerSizeCounter_.fetch_add(1);
        item->Younger = true;
    }
}

template <class TKey, class TValue, class THash>
void TAsyncSlruCacheBase<TKey, TValue, THash>::MoveToOlder(TShard* shard, TItem* item)
{
    YT_ASSERT(!item->Empty());
    item->Unlink();
    shard->OlderLruList.PushFront(item);
    if (item->Younger) {
        i64 weight = item->CachedWeight;
        shard->YoungerWeightCounter -= weight;
        YoungerWeightCounter_.fetch_sub(weight);
        YoungerSizeCounter_.fetch_sub(1);
        shard->OlderWeightCounter += weight;
        OlderWeightCounter_.fetch_add(weight);
        OlderSizeCounter_.fetch_add(1);
        item->Younger = false;
    }
}

template <class TKey, class TValue, class THash>
void TAsyncSlruCacheBase<TKey, TValue, THash>::Pop(TShard* shard, TItem* item)
{
    if (item->Empty()) {
        return;
    }

    i64 weight = item->CachedWeight;
    if (item->Younger) {
        shard->YoungerWeightCounter -= weight;
        YoungerWeightCounter_.fetch_sub(weight);
        YoungerSizeCounter_.fetch_sub(1);
    } else {
        shard->OlderWeightCounter -= weight;
        OlderWeightCounter_.fetch_sub(weight);
        OlderSizeCounter_.fetch_sub(1);
    }
    item->Unlink();
}

template <class TKey, class TValue, class THash>
std::vector<typename TAsyncSlruCacheBase<TKey, TValue, THash>::TValuePtr>
TAsyncSlruCacheBase<TKey, TValue, THash>::DoTrim(TShard* shard)
{
    // Move from older to younger.
    auto capacity = Capacity_.load();
    auto youngerSizeFraction = YoungerSizeFraction_.load();
    while (!shard->OlderLruList.Empty() &&
        Config_->ShardCount * shard->OlderWeightCounter > capacity * (1 - youngerSizeFraction))
    {
        auto* item = &*(--shard->OlderLruList.End());
        MoveToYounger(shard, item);
    }

    // Evict from younger.
    std::vector<TValuePtr> evictedValues;
    while (!shard->YoungerLruList.Empty() &&
        static_cast<i64>(Config_->ShardCount * (shard->YoungerWeightCounter + shard->OlderWeightCounter)) > capacity)
    {
        auto* item = &*(--shard->YoungerLruList.End());
        auto value = item->Value;

        Pop(shard, item);

        YT_VERIFY(shard->ItemMap.erase(value->GetKey()) == 1);
        --Size_;

        if (!IsResurrectionSupported()) {
            YT_VERIFY(shard->ValueMap.erase(value->GetKey()) == 1);
            value->Cache_.Reset();
        }

        YT_VERIFY(value->Item_ == item);
        value->Item_ = nullptr;

        evictedValues.push_back(std::move(value));

        delete item;
    }

    return evictedValues;
}

template <class TKey, class TValue, class THash>
void TAsyncSlruCacheBase<TKey, TValue, THash>::Trim(TShard* shard, NConcurrency::TSpinlockWriterGuard<NConcurrency::TReaderWriterSpinLock>& guard)
{
    auto evictedValues = DoTrim(shard);
    guard.Release();

    for (const auto& value : evictedValues) {
        OnRemoved(value);
    }
}

template <class TKey, class TValue, class THash>
void TAsyncSlruCacheBase<TKey, TValue, THash>::FinishInsertAndTrim(
        TShard* shard,
        NConcurrency::TSpinlockWriterGuard<NConcurrency::TReaderWriterSpinLock>& guard,
        const TValuePtr& insertedValue)
{
    auto evictedValues = DoTrim(shard);
    guard.Release();

    bool immediatelyRemoved = false;
    for (const auto& value : evictedValues) {
        if (value == insertedValue) {
            immediatelyRemoved = true;
            continue;
        }
        OnRemoved(value);
    }
    if (!immediatelyRemoved) {
        OnAdded(insertedValue);
    }
}

////////////////////////////////////////////////////////////////////////////////

template <class TKey, class TValue, class THash>
TAsyncSlruCacheBase<TKey, TValue, THash>::TInsertCookie::TInsertCookie()
    : Active_(false)
{ }

template <class TKey, class TValue, class THash>
TAsyncSlruCacheBase<TKey, TValue, THash>::TInsertCookie::TInsertCookie(const TKey& key)
    : Key_(key)
    , Active_(false)
{ }

template <class TKey, class TValue, class THash>
TAsyncSlruCacheBase<TKey, TValue, THash>::TInsertCookie::TInsertCookie(TInsertCookie&& other)
    : Key_(std::move(other.Key_))
    , Cache_(std::move(other.Cache_))
    , ValueFuture_(std::move(other.ValueFuture_))
    , Active_(other.Active_.exchange(false))
{ }

template <class TKey, class TValue, class THash>
TAsyncSlruCacheBase<TKey, TValue, THash>::TInsertCookie::~TInsertCookie()
{
    Abort();
}

template <class TKey, class TValue, class THash>
typename TAsyncSlruCacheBase<TKey, TValue, THash>::TInsertCookie& TAsyncSlruCacheBase<TKey, TValue, THash>::TInsertCookie::operator =(TInsertCookie&& other)
{
    if (this != &other) {
        Abort();
        Key_ = std::move(other.Key_);
        Cache_ = std::move(other.Cache_);
        ValueFuture_ = std::move(other.ValueFuture_);
        Active_ = other.Active_.exchange(false);
    }
    return *this;
}

template <class TKey, class TValue, class THash>
const TKey& TAsyncSlruCacheBase<TKey, TValue, THash>::TInsertCookie::GetKey() const
{
    return Key_;
}

template <class TKey, class TValue, class THash>
typename TAsyncSlruCacheBase<TKey, TValue, THash>::TValueFuture
TAsyncSlruCacheBase<TKey, TValue, THash>::TInsertCookie::GetValue() const
{
    YT_ASSERT(ValueFuture_);
    return ValueFuture_;
}

template <class TKey, class TValue, class THash>
bool TAsyncSlruCacheBase<TKey, TValue, THash>::TInsertCookie::IsActive() const
{
    return Active_;
}

template <class TKey, class TValue, class THash>
void TAsyncSlruCacheBase<TKey, TValue, THash>::TInsertCookie::Cancel(const TError& error)
{
    auto expected = true;
    if (!Active_.compare_exchange_strong(expected, false)) {
        return;
    }

    Cache_->CancelInsert(Key_, error);
}

template <class TKey, class TValue, class THash>
void TAsyncSlruCacheBase<TKey, TValue, THash>::TInsertCookie::EndInsert(TValuePtr value)
{
    auto expected = true;
    if (!Active_.compare_exchange_strong(expected, false)) {
        return;
    }

    Cache_->EndInsert(value);
}

template <class TKey, class TValue, class THash>
TAsyncSlruCacheBase<TKey, TValue, THash>::TInsertCookie::TInsertCookie(
    const TKey& key,
    TIntrusivePtr<TAsyncSlruCacheBase> cache,
    TValueFuture valueFuture,
    bool active)
    : Key_(key)
    , Cache_(std::move(cache))
    , ValueFuture_(std::move(valueFuture))
    , Active_(active)
{ }

template <class TKey, class TValue, class THash>
void TAsyncSlruCacheBase<TKey, TValue, THash>::TInsertCookie::Abort()
{
    Cancel(TError(NYT::EErrorCode::Canceled, "Cache item insertion aborted"));
}

////////////////////////////////////////////////////////////////////////////////

template <class TKey, class TValue, class THash>
TMemoryTrackingAsyncSlruCacheBase<TKey, TValue, THash>::TMemoryTrackingAsyncSlruCacheBase(
    TSlruCacheConfigPtr config,
    IMemoryUsageTrackerPtr memoryTracker,
    const NProfiling::TProfiler& profiler)
    : TAsyncSlruCacheBase<TKey, TValue, THash>(
        std::move(config),
        profiler)
    , MemoryTracker_(std::move(memoryTracker))
{
    MemoryTracker_->SetLimit(this->Capacity_.load());
}

template <class TKey, class TValue, class THash>
TMemoryTrackingAsyncSlruCacheBase<TKey, TValue, THash>::~TMemoryTrackingAsyncSlruCacheBase()
{
    MemoryTracker_->SetLimit(0);
}

template <class TKey, class TValue, class THash>
void TMemoryTrackingAsyncSlruCacheBase<TKey, TValue, THash>::OnAdded(const TValuePtr& value)
{
    MemoryTracker_->Acquire(this->GetWeight(value));
}

template <class TKey, class TValue, class THash>
void TMemoryTrackingAsyncSlruCacheBase<TKey, TValue, THash>::OnRemoved(const TValuePtr& value)
{
    MemoryTracker_->Release(this->GetWeight(value));
}

template <class TKey, class TValue, class THash>
void TMemoryTrackingAsyncSlruCacheBase<TKey, TValue, THash>::Reconfigure(const TSlruCacheDynamicConfigPtr& config)
{
    if (auto newCapacity = config->Capacity) {
        MemoryTracker_->SetLimit(*newCapacity);
    }
    TAsyncSlruCacheBase<TKey, TValue, THash>::Reconfigure(config);
}

////////////////////////////////////////////////////////////////////////////////

} // namespace NYT
