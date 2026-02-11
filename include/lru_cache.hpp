#ifndef LRU_CACHE_HPP
#define LRU_CACHE_HPP

#include <list>
#include <stdexcept>
#include <unordered_map>

template <typename Key, typename Value>
class ICacheEventHandler {
public:
    virtual ~ICacheEventHandler() {}

    // Called when an item is being removed from memory
    virtual bool onEvict(const Key& key, Value& value) = 0;
};

template <typename Key, typename Value>
class LRUCache {
public:
    LRUCache(size_t capacity, ICacheEventHandler<Key, Value>* handler = nullptr)
        : capacity(capacity), handler(handler) {
        if (capacity == 0)
            throw std::invalid_argument("Cache capacity must be a positive value.");
    }

    size_t size() const { return cacheMap.size(); }

    Value* get(const Key& key) {
        typename std::unordered_map<Key, CacheEntry>::iterator it = cacheMap.find(key);

        if (it == cacheMap.end()) return nullptr;

        lruList.erase(it->second.listIt);
        lruList.push_front(key);

        it->second.listIt = lruList.begin();

        return &(it->second.value);
    }

    Value* put(const Key& key, Value&& value) {
        typename std::unordered_map<Key, CacheEntry>::iterator it = cacheMap.find(key);

        if (it != cacheMap.end()) {
            it->second.value = std::move(value);
            lruList.erase(it->second.listIt);
            lruList.push_front(key);

            it->second.listIt = lruList.begin();

            return &(it->second.value);
        }

        if (cacheMap.size() >= capacity && !evict()) return nullptr;

        lruList.push_front(key);

        cacheMap[key] = { std::move(value), lruList.begin() };

        // Return the pointer to the newly moved-in value
        return &(cacheMap[key].value);
    }

    Value* put(const Key& key, const Value& value) {
        typename std::unordered_map<Key, CacheEntry>::iterator it = cacheMap.find(key);

        if (it != cacheMap.end()) {
            it->second.value = value;
            lruList.erase(it->second.listIt);
            lruList.push_front(key);

            it->second.listIt = lruList.begin();

            return &(it->second.value);
        }

        if (cacheMap.size() >= capacity && !evict()) return nullptr;

        lruList.push_front(key);

        cacheMap[key] = { value, lruList.begin() };

        return &(cacheMap[key].value);
    }

    void remove(const Key& key) {
        typename std::unordered_map<Key, CacheEntry>::iterator it = cacheMap.find(key);

        if (it != cacheMap.end()) {
            if (handler && !handler->onEvict(key, it->second.value)) {
                // Abort removal
                return;
            }

            lruList.erase(it->second.listIt);

            cacheMap.erase(it);
        }
    }

    template <typename Func>
    void forEach(Func func) {
        typename std::unordered_map<Key, CacheEntry>::iterator it = cacheMap.begin();

        for (; it != cacheMap.end(); ++it) {
            func(it->first, it->second.value);
        }
    }

public:
    struct CacheEntry {
        Value value;
        typename std::list<Key>::iterator listIt;
    };

private:
    bool evict() {
        Key lastKey = lruList.back();

        if (handler && !handler->onEvict(lastKey, cacheMap[lastKey].value)) {
            // Abort eviction
            return false;
        }

        cacheMap.erase(lastKey);
        lruList.pop_back();

        return true;
    }

private:
    std::unordered_map<Key, CacheEntry> cacheMap;

    std::list<Key> lruList;

    size_t capacity;

    ICacheEventHandler<Key, Value>* handler;
};

#endif