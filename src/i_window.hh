#pragma once
#include <string>

namespace pce::sdlos {

    class IWindow {
    public:
        virtual ~IWindow() = default;

        virtual void show()  = 0;
        virtual void hide()  = 0;

        virtual void resize(int width, int height) = 0;
        virtual void move(int x, int y)            = 0;

        virtual void focus() = 0;

        virtual void minimize() = 0;
        virtual void maximize() = 0;
        virtual void restore()  = 0;

        virtual bool        isFocused()   const = 0;
        virtual bool        isMinimized() const = 0;
        virtual bool        isMaximized() const = 0;
        virtual std::string getTitle()    const = 0;
        virtual int         getId()       const = 0;
    };

} // namespace pce::sdlos
