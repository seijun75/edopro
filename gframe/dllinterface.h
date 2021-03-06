#ifndef DLL_INTERFACE_H_
#define DLL_INTERFACE_H_
#ifndef YGOPRO_BUILD_DLL
#include <ocgapi.h>
#else
#include "ocgapi_types.h"
#include "text_types.h"
bool ReloadCore(void* handle);
void UnloadCore(void *handle);
void* LoadOCGcore(const path_string& path);
void* ChangeOCGcore(const path_string& path, void* handle);

#define X(type,name,...) extern type(*name)(__VA_ARGS__);
#include "ocgcore_functions.inl"
#undef X

#endif //YGOPRO_BUILD_DLL
#endif /* DLL_INTERFACE_H_ */