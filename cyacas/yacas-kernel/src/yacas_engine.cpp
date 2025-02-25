/*
 * This file is part of yacas.
 * Yacas is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Lesset General Public License as
 * published by the Free Software Foundation, either version 2.1
 * of the License, or (at your option) any later version.
 *
 * Yacas is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with yacas. If not, see <http://www.gnu.org/licenses/>.
 *
 */

/*
 * File:   yacas_engine.cpp
 * Author: mazur
 *
 * Created on November 7, 2015, 12:52 PM
 */

#include "yacas_engine.hpp"

#include <json/writer.h>
#include <zmq_addon.hpp>

YacasEngine::YacasEngine(const std::string& scripts_path,
                         zmq::context_t& ctx,
                         const std::string& endpoint) :
    _yacas(_side_effects),
    _socket(ctx, zmq::socket_type::pair),
    _shutdown(false)
{
    _yacas.Evaluate(std::string("DefaultDirectory(\"") + scripts_path +
                    std::string("\");"));
    _yacas.Evaluate("Load(\"yacasinit.ys\");");
    _yacas.Evaluate("Plot2D'outputs();");
    _yacas.Evaluate("UnProtect(Plot2D'outputs);");
    _yacas.Evaluate("Plot2D'outputs() := {{\"default\", \"png\"}, {\"png\", \"Plot2D'png\"}}");
    _yacas.Evaluate("Protect(Plot2D'outputs);");
    _yacas.Evaluate("Plot3DS'outputs();");
    _yacas.Evaluate("UnProtect(Plot3DS'outputs);");
    _yacas.Evaluate("Plot3DS'outputs() := {{\"default\", \"png\"}, {\"png\", \"Plot3DS'png\"}}");
    _yacas.Evaluate("Protect(Plot3DS'outputs);");

    _socket.connect(endpoint);

    _worker_thread = new std::thread(std::bind(&YacasEngine::_worker, this));
}

YacasEngine::~YacasEngine()
{
    _shutdown = true;
    _yacas.getDefEnv().getEnv().stop_evaluation = true;
    _cv.notify_all();
    _worker_thread->join();

    delete _worker_thread;
}

void YacasEngine::submit(unsigned long id, const std::string& expr)
{
    const TaskInfo ti = {id, expr};

    std::lock_guard<std::mutex> lock(_mtx);
    _tasks.push_back(ti);
    _cv.notify_all();
}

void YacasEngine::_worker()
{
    for (;;) {
        TaskInfo ti;

        {
            std::unique_lock<std::mutex> lock(_mtx);

            while (_tasks.empty() && !_shutdown)
                _cv.wait(lock);

            if (_shutdown)
                return;

            ti = _tasks.front();
            _tasks.pop_front();
        }

        Json::Value calculate_content;
        calculate_content["id"] = Json::Value::UInt64(ti.id);
        calculate_content["expr"] = ti.expr;
        zmq::multipart_t status_msg;
        status_msg.addstr("calculate");
        status_msg.addstr(Json::writeString(Json::StreamWriterBuilder(),
                                        calculate_content));
        status_msg.send(_socket);

        _side_effects.clear();
        _side_effects.str("");

        _yacas.Evaluate((ti.expr + ";"));

        Json::Value result_content;
        result_content["id"] = Json::Value::UInt64(ti.id);

        if (_yacas.IsError())
            result_content["error"] = _yacas.Error();
        else
            result_content["result"] = _yacas.Result();

        result_content["side_effects"] = _side_effects.str();

        zmq::multipart_t result_msg;
        result_msg.addstr("result");
        result_msg.addstr(Json::writeString(Json::StreamWriterBuilder(),
                                        result_content));
        result_msg.send(_socket);
    }
}
