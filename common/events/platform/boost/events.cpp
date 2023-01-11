// Copyright 2023 Northern.tech AS
//
//    Licensed under the Apache License, Version 2.0 (the "License");
//    you may not use this file except in compliance with the License.
//    You may obtain a copy of the License at
//
//        http://www.apache.org/licenses/LICENSE-2.0
//
//    Unless required by applicable law or agreed to in writing, software
//    distributed under the License is distributed on an "AS IS" BASIS,
//    WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
//    See the License for the specific language governing permissions and
//    limitations under the License.

#include <common/events.hpp>

#include <boost/asio.hpp>

namespace asio = boost::asio;

namespace mender::events {

void EventLoop::Run() {
	ctx_.run();
}

void EventLoop::Stop() {
	ctx_.stop();
}

Timer::Timer(EventLoop &loop) :
	timer_(loop.ctx_) {
}

void Timer::Cancel() {
	timer_.cancel();
}

} // namespace mender::events
