#pragma once

// Set while SharedHttpEndpoint is applying a REST POST (issue #15).
inline thread_local bool g_restModulePostActive = false;
