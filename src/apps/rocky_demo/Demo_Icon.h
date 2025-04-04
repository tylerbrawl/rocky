/**
 * rocky c++
 * Copyright 2023 Pelican Mapping
 * MIT License
 */
#pragma once
#include <rocky/vsg/ecs.h>
#include "helpers.h"

using namespace ROCKY_NAMESPACE;

auto Demo_Icon = [](Application& app)
{
    static entt::entity entity = entt::null;
    static Status status;

    if (status.failed())
    {
        ImGui::TextColored(ImVec4(1, 0, 0, 1), "Image load failed");
        ImGui::TextColored(ImVec4(1, 0, 0, 1), status.message.c_str());
        return;
    }

    if (entity == entt::null)
    {
        auto [lock, registry] = app.registry.write();

        // Load an icon image
        auto io = app.context->io;
        auto image = io.services.readImageFromURI("https://readymap.org/readymap/filemanager/download/public/icons/BENDER.png", io);
        if (image.status.failed())
        {
            status = image.status;
            return;
        }

        // Make an entity to host our icon:
        entity = registry.create();

        // Attach the new Icon and set up its properties:
        auto& icon = registry.emplace<Icon>(entity);
        icon.image = image.value;
        icon.style = IconStyle{ 75, 0.0f }; // pixel size, rotation(radians)

        // Transform to place the icon:
        auto& transform = registry.emplace<Transform>(entity);
        transform.position = GeoPoint(SRS::WGS84, 0, 0, 50000);
        transform.topocentric = false; // optimization for billboards :)
    }

    if (ImGuiLTable::Begin("icon"))
    {
        auto [lock, registry] = app.registry.read();

        bool visible = ecs::visible(registry, entity);
        if (ImGuiLTable::Checkbox("Show", &visible))
            ecs::setVisible(registry, entity, visible);

        auto& icon = registry.get<Icon>(entity);

        if (ImGuiLTable::SliderFloat("Pixel size", &icon.style.size_pixels, 1.0f, 1024.0f))
            icon.revision++;

        if (ImGuiLTable::SliderFloat("Rotation", &icon.style.rotation_radians, 0.0f, 6.28f))
            icon.revision++;

        ImGuiLTable::End();
    }
};
