cmake_minimum_required(VERSION 3.5)

# ------------------------------------------------------------------------------
# Configure directories
# ------------------------------------------------------------------------------
set(GREX_PROJECTS_COMMON_DIR "${GREX_PROJECTS_DIR}/common")

if (GREX_ENABLE_D3D12)
    add_subdirectory(201_pbr_spheres_d3d12)
    add_subdirectory(202_pbr_camera_d3d12)
    add_subdirectory(203_pbr_align_d3d12)
    add_subdirectory(251_pbr_explorer_d3d12)
    add_subdirectory(252_pbr_material_properties_d3d12)
    add_subdirectory(253_pbr_material_textures_d3d12)
endif()

if (GREX_ENABLE_METAL)
    add_subdirectory(201_pbr_spheres_metal)
    add_subdirectory(202_pbr_camera_metal)
    add_subdirectory(203_pbr_align_metal)
    add_subdirectory(251_pbr_explorer_metal)
    add_subdirectory(252_pbr_material_properties_metal)
    add_subdirectory(253_pbr_material_textures_metal)
endif()

if (GREX_ENABLE_VULKAN)
    add_subdirectory(201_pbr_spheres_vulkan)
    add_subdirectory(202_pbr_camera_vulkan)
    add_subdirectory(203_pbr_align_vulkan)
    add_subdirectory(251_pbr_explorer_vulkan)
    add_subdirectory(252_pbr_material_properties_vulkan)
    add_subdirectory(253_pbr_material_textures_vulkan)
endif()
