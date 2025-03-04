cmake_minimum_required(VERSION 3.5)

project(104_debug_tbn_vulkan)

add_executable(
    104_debug_tbn_vulkan
    104_debug_tbn_vulkan.cpp
    ${GREX_PROJECTS_COMMON_DIR}/config.h
    ${GREX_PROJECTS_COMMON_DIR}/vk_renderer.h
    ${GREX_PROJECTS_COMMON_DIR}/vk_renderer.cpp
    ${GREX_PROJECTS_COMMON_DIR}/window.h
    ${GREX_PROJECTS_COMMON_DIR}/window.cpp
    ${GREX_PROJECTS_COMMON_DIR}/tri_mesh.h
    ${GREX_PROJECTS_COMMON_DIR}/tri_mesh.cpp
    ${IMGUI_VULKAN_FILES}
    ${GREX_THIRD_PARTY_DIR}/glslang/StandAlone/resource_limits_c.cpp
    ${GREX_THIRD_PARTY_DIR}/glslang/StandAlone/ResourceLimits.cpp
    ${GREX_THIRD_PARTY_DIR}/MikkTSpace/mikktspace.h
    ${GREX_THIRD_PARTY_DIR}/MikkTSpace/mikktspace.c
)

set_target_properties(104_debug_tbn_vulkan PROPERTIES FOLDER "geometry")

target_compile_definitions(
    104_debug_tbn_vulkan
    PUBLIC ENABLE_IMGUI_VULKAN
           TRIMESH_USE_MIKKTSPACE
)

target_include_directories(
    104_debug_tbn_vulkan
    PUBLIC ${GREX_PROJECTS_COMMON_DIR}
           ${GREX_THIRD_PARTY_DIR}/glslang # This needs to come before ${VULKAN_INCLUDE_DIR}
           ${VULKAN_INCLUDE_DIR}
           ${GREX_THIRD_PARTY_DIR}/VulkanMemoryAllocator/include
           ${GREX_THIRD_PARTY_DIR}/glm
           ${GREX_THIRD_PARTY_DIR}/tinyobjloader
           ${GREX_THIRD_PARTY_DIR}/stb
           ${IMGUI_INC_DIR}
           ${GREX_THIRD_PARTY_DIR}/MikkTSpace
)

target_link_libraries(
    104_debug_tbn_vulkan
    PUBLIC glfw
           glslang
           SPIRV
           dxcompiler
)

if(WIN32)
    target_compile_definitions(
		104_debug_tbn_vulkan
		PUBLIC VK_USE_PLATFORM_WIN32_KHR
	)
	target_link_libraries(
		104_debug_tbn_vulkan
		PUBLIC "${VULKAN_LIBRARY_DIR}/vulkan-1.lib"
	)
endif()
