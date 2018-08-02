//
// Created by dy2018 on 18-8-2.
//
#include "../http_server.hpp"
#ifndef CINATRA_TESTCONTROLLER_HPP
#define CINATRA_TESTCONTROLLER_HPP
class test_controller
{
public:
    void method(cinatra::request& req,cinatra::response& res)
    {
        res.render_string(std::to_string(value));
        ++value;
    }

private:
    int value = 0;
};
#endif //CINATRA_TESTCONTROLLER_HPP
