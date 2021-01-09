#include "header_obj.h"
#include "class.h"
#include "vm.h"

// TODO: 待后续解释
DEFINE_BUFFER_METHOD(Value)

// 初始化对象头
void initObjHeader(VM *vm, ObjHeader *objHeader, ObjType objType, Class *class) {
    objHeader->type = objType;
    // 对象是否可达初始化为 false，其值最终由垃圾回收机制设置
    objHeader->isAccess = false;
    // 设置成 meta 类
    objHeader->class = class;
    // 初始化的 objHeader 的 next 指向当前所有已分配对象链表的首节点
    objHeader->next = vm->allObjects;
    // 然后再将初始化的 objHeader 设为当前所有已分配对象链表的首节点
    // 这两步操作就是为了将初始化的 objHeader 插入到已分配对象链表的表头
    vm->allObjects = objHeader;
}
