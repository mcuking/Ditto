#include "compiler.h"
#include "obj_list.h"

// 释放 obj 自身及其占用的内存
void freeObject(VM *vm, ObjHeader *obj) {
    // 根据对象类型分别处理
    switch (obj->type) {
        case OT_CLASS:
            MethodBufferClear(vm, &((Class *)obj)->methods);
            break;

        case OT_THREAD: {
            ObjThread *objThread = (ObjThread *)obj;
            DEALLOCATE(vm, objThread->frames);
            DEALLOCATE(vm, objThread->stack);
            break;
        }

        case OT_FUNCTION: {
            ObjFn *fn = (ObjFn *)obj;
            ValueBufferClear(vm, &fn->constants);
            ByteBufferClear(vm, &fn->instrStream);
            break;
        }

        case OT_LIST:
            ValueBufferClear(vm, &((ObjList *)obj)->elements);
            break;

        case OT_MAP:
            DEALLOCATE(vm, ((ObjMap *)obj)->entries);
            break;

        case OT_MODULE:
            StringBufferClear(vm, &((ObjModule *)obj)->moduleVarName);
            ValueBufferClear(vm, &((ObjModule *)obj)->moduleVarValue);
            break;

        case OT_STRING:
        case OT_RANGE:
        case OT_CLOSURE:
        case OT_INSTANCE:
        case OT_UPVALUE:
            break;
    }

    // 最后再释放自己
    DEALLOCATE(vm, obj);
}
