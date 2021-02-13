#include "obj_map.h"
#include "class.h"
#include "obj_range.h"
#include "obj_string.h"

// 新建 map 对象
ObjMap *newObjMap(VM *vm) {
    // 分配内存
    ObjMap *objMap = ALLOCATE(vm, ObjMap);

    // 申请内存失败
    if (objMap == NULL) {
        MEM_ERROR("allocate ObjMap failed!");
    }

    // 初始化对象头
    initObjHeader(vm, &objMap->objHeader, OT_MAP, vm->mapClass);

    objMap->capacity = objMap->count = 0;
    objMap->entries = NULL;

    return objMap;
}

// 计算数字的哈希值
static uint32_t hashNum(double num) {
    Bits64 bits64;
    bits64.num = num;
    // num 的高 32 位和低 32 位异或的结果作为 num 的哈希值
    return bits64.bits32[0] ^ bits64.bits32[1];
}

// 计算对象的哈希值
static uint32_t hashObj(ObjHeader *objHeader) {
    switch (objHeader->type) {
        case OT_STRING:
            // 直接返回 string 对象的 hashCode
            return ((ObjString *)objHeader)->hashCode;
        case OT_RANGE: {
            // 强制类型转换成 range 对象
            ObjRange *objRange = (ObjRange *)objHeader;
            // 返回 range 对象的 from 和 to 的哈希值再做异或的值
            return hashNum(objRange->from) ^ hashNum(objRange->to);
        }
        case OT_CLASS: {
            // 强制类型转换成 class 对象
            Class *class = (Class *)objHeader;
            // 返回 class 对象的 name 字符串的哈希值
            return hashString(class->name->value.start, class->name->value.length);
        }
        default:
            RUN_ERROR("the hashable needs be objString, objRange and class.");
    }
    return 0;
}

// 根据 value 的类型调用相应的方法计算其哈希值
static uint32_t hashValue(Value value) {
    switch (value.type) {
        case VT_FALSE:
            return 0;
        case VT_NULL:
            return 1;
        case VT_TRUE:
            return 2;
        case VT_NUM:
            return hashNum(value.num);
        case VT_OBJ:
            return hashObj(value.objHeader);
        default:
            RUN_ERROR("Not support type dashed!");
    }
    return 0;
}

// 向 entries 中添加 entry
// 先根据 key 的哈希值按照一定算法计算在 entries 中的位置 index（下面方法是 key 的哈希值和 entries 的容量 capacity 取模来得到在 entries 中的位置 index）
// 会遇到三种情况：
// 1. index 对应位置没有被使用过，则正常赋值 entry 的 key 和 value ，并返回 true 即可
// 2. index 对应位置被使用过了，但已存在的 entry 的 key 和传入的 key 相同，则将 entry 的 value 覆盖成传入的，并返回 false 即可
// 3. index 对应位置被使用过了，且已存在的 entry 的 key 和传入的 key 不同，即不同的 key 计算出来的哈希值是相同的，就会发生冲突，可采用开放探测法解决，
// 例如 index 加 1 再和 entries 的容量 capacity 取模，探测下一个位置是否可以存储
static bool addEntry(Entry *entries, uint32_t capacity, Value key, Value value) {
    // key 的哈希值和 entries 的容量 capacity 取模得到在 entries 中的位置
    uint32_t index = hashValue(key) % capacity;

    // 通过开放探测法找到可用的 slot
    while (true) {
        // 1. 如果 slot 中的 entry 的 key 类型为 VT_UNDEFINED，则说明为空闲 slot，直接赋值即可
        if (entries[index].key.type == VT_UNDEFINED) {
            entries[index].key = key;
            entries[index].value = value;
            return true;
        }
        // 2. 如果 slot 中的 entry 的 key 和传入的 key 相同，则说明该 slot 就是存储该 key-value 的 slot，直接覆盖 value 即可
        else if (valueIsEqual(entries[index].key, key)) {
            entries[index].value = value;
            return false;
        }

        // 3. 如果以上两个情况不满足，则 index 加 1 再和 entries 的容量 capacity 取模
        // 得出来的值作为下一次循环中的位置 index，探测下一个位置是否可以存储
        index = (index + 1) % capacity;
    }
}

// 在 objMap 中查找 key 对应的 entry
static Entry *findEntry(ObjMap *objMap, Value key) {
    // 如果 objMap 为空，则返回 null
    if (objMap->capacity == 0) {
        return NULL;
    }

    // 根据 key 计算对应的 entry 在 entries 中的位置，即槽位（slot）
    uint32_t index = hashValue(key) % objMap->capacity;
    Entry *entry;
    while (true) {
        entry = &objMap->entries[index];

        // 如果该 entry 的 key 和传入的 key 相等，则说明就是该 entry
        if (valueIsEqual(entry->key, key)) {
            return entry;
        }

        // 当 entry 的 key 类型为 undefined
        // 且 entry 的 value 类型为 false，则说明探测链结束
        // 背景知识如下：
        // 当存储的多个 entry 中的 key 的哈希值相同时，就会产生冲突探测链，具体可参考上面的 addEntry
        // 此时查找时，依然根据 entry 的 key 的哈希值计算 entry 在 entries 上的位置 index
        // 如果没有，则 index 加 1 继续沿着冲突探测链向下寻找
        // 但当遇到某个一个 entry 被删除了，这个链条就断掉了
        // 假设某个 entry 就是存储在被删除的 entry 的后面，则无法找到该 entry
        // 为了解决这个问题，决定采用伪删除：
        // 开始时我们会将所有的 entry 的 value 的 type 初始化成 VT_FALSE
        // 当删除某个 entry 时，则将该 entry 的 value 的 type 设置成 VT_TRUE
        // 当查找某个 key 对应的 entry 时，在冲突探测链中，当 entry 的 value 的 type 为 VT_TRUE，说明冲突探测链
        // 没有断，可以继续循环顺着探测链继续往下找（前提是 entry 的 key 的 type 为 VT_UNDEFINED）
        // 如果遇到下一个 entry 的 value 的 type 为 VT_FALSE，说明这个 entry 是没有被用来存储的，即已经走到了探测链的尾部
        if (VALUE_IS_UNDEFINED(entry->key) && VALUE_IS_FALSE(entry->value)) {
            return NULL;
        }

        // 继续沿着探测链往下探测
        index = (index + 1) % objMap->capacity;
    }
}

// 将 objMap 的容量调整到 newCapacity
// 之所以要将原有数据复制到新的空间中，再回收旧空间，而不是在旧空间的基础上扩容，
// 原因是哈希遍存储数据的方式不是线性的，数据所在的槽位 slot 是利用线性结构的容量取模得到的，属于离散分布
// 当在原有的旧空间基础上扩容，容量就变化了，根据新容量取模计算得到的槽位 slot 的位置就不对了，也就是找不到原来的数据了
// 所以只能将原有的数据拷贝到新的空间，再回收旧空间
static void resizeMap(VM *vm, ObjMap *objMap, uint32_t newCapacity) {
    // 1. 先新建一个 entry 数组
    Entry *newEntries = ALLOCATE_ARRAY(vm, Entry, newCapacity);
    uint32_t idx = 0;
    while (idx < newCapacity) {
        newEntries[idx].key = VT_TO_VALUE(VT_UNDEFINED); // entry 的 key 的 type 初始化为 VT_UNDEFINED
        newEntries[idx].value = VT_TO_VALUE(VT_FALSE);   // entry 的 value 的 type 初始化为 VT_FALSE，用于和删除的槽位作区分（删除的 value 的 type 设置成 VT_TRUE）
        idx++;
    }

    // 2. 在遍历老的 entry 数组，将有值的部分插入到新的数组中
    if (objMap->count > 0) {
        idx = 0;
        while (idx < objMap->capacity) {
            // 如果该槽位 slot 有值，则将值插入到新的数组中
            if (objMap->entries[idx].key.type != VT_UNDEFINED) {
                addEntry(newEntries, newCapacity, objMap->entries[idx].key, objMap->entries[idx].value);
            }
            idx++;
        }
    }

    // 3. 将老的 entries 数组所占内存回收
    DEALLOCATE_ARRAY(vm, objMap->entries, objMap->count);

    objMap->entries = newEntries;   // 更新 entry 数组
    objMap->capacity = newCapacity; // 更新容量
}

// 向 map 对象的键值为 key 的地方设置值 value
void mapSet(VM *vm, ObjMap *objMap, Value key, Value value) {
    // 如果新增一个 entry 后，容量利用率超过 80 % 时，就需要扩容
    if (objMap->count + 1 > objMap->capacity * MAP_LOAD_PERCENT) {
        uint32_t newCapacity = objMap->capacity * CAPACITY_GROW_FACTOR; // 新空间为到旧空间的 4 倍
        // 如果小于容量最小值 64，则按照最小值设置
        if (newCapacity < MIN_CAPACITY) {
            newCapacity = MIN_CAPACITY;
        }

        resizeMap(vm, objMap, newCapacity);
    }

    // 判断是新增的 entry，还是覆盖原有的 entry
    bool isNewAdd = addEntry(objMap->entries, objMap->capacity, key, value);
    // 如果创建了新的 key 则 objMap->count 加 1
    if (isNewAdd) {
        objMap->count++;
    }
}

// 获取 map 对象的键值为 key 的地方的值
Value mapGet(ObjMap *objMap, Value key) {
    Entry *entry = findEntry(objMap, key);

    // 如果 map 对象中没有找到 key 对应的 entry，则返回 undefined
    if (entry == NULL) {
        return VT_TO_VALUE(VT_UNDEFINED);
    }

    // 找到则返回 key 对应的 value
    return entry->value;
}

// 删除 map 对象的键值为 key 的地方的值
Value removeKey(VM *vm, ObjMap *objMap, Value key) {
    Entry *entry = findEntry(objMap, key);

    // 如果没有 key 对应的值则返回 NULL
    if (entry == NULL) {
        return VT_TO_VALUE(VT_NULL);
    }

    Value value = entry->value;
    entry->key = VT_TO_VALUE(VT_UNDEFINED); // 将 entry 的 key 的 type 设置成 VT_UNDEFINED
    entry->value = VT_TO_VALUE(VT_TRUE);    // 将 entry 的 key 的 type 设置成 VT_TRUE，用于在冲突探测链中标记此处槽位 slot 为删除，而非未使用过
    objMap->count--;

    // 如果删除后 objMap 为空，则回收内存空间
    if (objMap->count == 0) {
        clearMap(vm, objMap);
    }
    // 如果删除后实际使用槽位 slot 数量小于容量的 1 / 4 的 80%，且实际使用量仍大于规定的最小容量，则缩小容量
    else if ((objMap->count < objMap->capacity / CAPACITY_GROW_FACTOR * MAP_LOAD_PERCENT) && objMap->count > MIN_CAPACITY) {
        uint32_t newCapacity = objMap->capacity / CAPACITY_GROW_FACTOR;

        // 如果缩小的新容量小于最小容量，则设置为最小容量
        if (newCapacity < MIN_CAPACITY) {
            newCapacity = MIN_CAPACITY;
        }

        resizeMap(vm, objMap, newCapacity);
    }

    return value;
}

// 删除 map 对象，即收回 map 对象占用的内存
void clearMap(VM *vm, ObjMap *objMap) {
    DEALLOCATE_ARRAY(vm, objMap->entries, objMap->count);
    objMap->entries = NULL;
    objMap->count = objMap->capacity = 0;
}
