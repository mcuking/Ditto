#include <string.h>
#include "meta_obj.h"
#include "obj_string.h"
#include "vm.h"
#include "class.h"

// 新建模块对象
ObjModule *newObjModule(VM *vm, const char *modName)
{
    // 申请内存
    ObjModule *objModule = ALLOCATE(vm, ObjModule);

    // 内存申请失败
    if (objModule == NULL)
    {
        MEM_ERROR("allocate ObjModule failed!");
    }

    /** 1. 设置 name **/
    objModule->name = NULL;
    if (modName != NULL)
    {
        objModule->name = newObjString(vm, modName, strlen(modName));
    }

    /** 2. 设置 objHeader **/
    // 注意：&objModule->objHeader 中 -> 优先级高于 &
    // 所以是取的 objHeader，然后再获取它的地址
    // 另外 ObjModule 时元信息对象，不属于任何一个类，
    // 所以最后一个参数传 NULL
    initObjHeader(vm, &objModule->objHeader, OT_MODULE, NULL);

    /** 3. 设置 moduleName **/
    // TODO: 待后续解释
    StringBufferInit(&objModule->moduleVarName);

    /** 4. 设置 moduleValue **/
    // TODO: 待后续解释
    ValueBufferInit(&objModule->moduleVarValue);

    return objModule;
}

// 新建实例对象
ObjInstance *newObjInstance(VM *vm, Class *class)
{
    // 申请内存
    // 注意：其中实例对象的属性个数保存在生成它的类中
    // 因为 objInstance->fields 是一个数组，需要额外内存存储字符串本身数据，
    // 所以使用 ALLOCATE_EXTRA 来申请内存
    ObjInstance *objInstance =
        ALLOCATE_EXTRA(vm, ObjInstance, sizeof(Value) * class->fieldNum);

    // 内存申请失败
    if (objInstance == NULL)
    {
        MEM_ERROR("allocate ObjInstance failed!");
    }

    /** 1. 设置 objHeader **/
    // 注意：&objInstance->objHeader 中 -> 优先级高于 &
    // 所以是取的 objHeader，然后再获取它的地址
    initObjHeader(vm, &objInstance->objHeader, OT_INSTANCE, class);

    /** 2. 设置 fields **/
    // 根据存放在所属类中的属性数量，
    // 来循环遍历进行初始化实例对象的属性值
    uint32_t idx = 0;
    while (idx < class->fieldNum)
    {
        objInstance->fields[idx] = VT_TO_VALUE(VT_NULL);
    }

    return objInstance;
}
