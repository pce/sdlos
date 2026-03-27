#pragma once
#include <string>
#include <vector>
#include <memory>

namespace pce::sdlos {


class Widget {
public:
    std::string id;
    std::string type;
    std::string content;
    int x, y, width, height;
    bool visible;
    bool enabled;

    Widget(const std::string& id, const std::string& type);
    virtual ~Widget() = default;

    virtual void render(SDL_Renderer* renderer) = 0;
    virtual void handleEvent(const SDL_Event& event) = 0;
    virtual void update() = 0;
};

class WidgetManager {
private:
    std::unordered_map<std::string, std::shared_ptr<Widget>> widgets;

public:
    void addWidget(const std::shared_ptr<Widget>& widget);
    void removeWidget(const std::string& id);
    std::shared_ptr<Widget> getWidget(const std::string& id);
    void renderAll(SDL_Renderer* renderer);
    void updateAll();
    void handleEvent(const SDL_Event& event);
};

}
