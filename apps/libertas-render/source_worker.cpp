// SPDX-FileCopyrightText: Libertas-Studio contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#include "source_worker.hpp"

#include <chrono>
#include <csignal>
#include <lcore/log.hpp>
#include <sys/wait.h>

SourceWorker::SourceWorker(const ldoc::Source& source, Callback callback,
                           std::function<void(std::span<const float>)> audio)
    : thread_([source, callback = std::move(callback),
               audio = std::move(audio)](std::stop_token stop) {
          while (!stop.stop_requested()) {
              pid_t child = -1;
              try {
                  auto ends = lipc::Channel::pair();
                  if (!ends)
                      throw std::runtime_error(ends.error());
                  auto spawned = lcomp::spawn_renderer(LIBERTAS_SOURCE_EXE,
                                                       std::move(ends->second).take_socket());
                  if (!spawned)
                      throw std::runtime_error(spawned.error());
                  child = *spawned;
                  lcomp::PreviewLink link(std::move(ends->first));
                  auto hello = link.take_hello(5000);
                  if (!hello)
                      throw std::runtime_error(hello.error());
                  auto project = ldoc::make_default_project("Source worker");
                  project.sources = {source};
                  auto sent = link.send_project(project);
                  if (!sent)
                      throw std::runtime_error(sent.error());
                  auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
                  while (!stop.stop_requested()) {
                      auto event = link.receive(100);
                      if (!event)
                          throw std::runtime_error(event.error());
                      if (!*event) {
                          if (std::chrono::steady_clock::now() > deadline)
                              throw std::runtime_error("source worker timed out");
                          continue;
                      }
                      auto& value = **event;
                      if (value.kind == lcomp::PreviewEvent::Kind::Pcm && audio)
                          audio(value.pcm.samples);
                      if (value.kind == lcomp::PreviewEvent::Kind::PeerGone)
                          throw std::runtime_error("source worker exited");
                      if (value.kind == lcomp::PreviewEvent::Kind::Fault)
                          throw std::runtime_error(value.fault);
                      if (value.kind == lcomp::PreviewEvent::Kind::Frame) {
                          if (value.frame.status == lcomp::PictureStatus::Complete)
                              callback(
                                  std::make_shared<lcomp::PreviewFrame>(std::move(value.frame)));
                          else
                              callback({});
                      }
                      if (value.kind == lcomp::PreviewEvent::Kind::Ack) {
                          std::this_thread::sleep_for(std::chrono::milliseconds(16));
                          auto refresh = link.refresh();
                          if (!refresh)
                              throw std::runtime_error(refresh.error());
                          deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
                      }
                  }
                  link.quit();
                  link.close();
              } catch (const std::exception& e) {
                  lc::warn("source {}: {}", source.name, e.what());
              }
              callback({});
              if (child > 0) {
                  int status = 0;
                  bool reaped = false;
                  for (int i = 0; i < 50; ++i) {
                      if (::waitpid(child, &status, WNOHANG) != 0) {
                          reaped = true;
                          break;
                      }
                      std::this_thread::sleep_for(std::chrono::milliseconds(10));
                  }
                  if (!reaped) {
                      ::kill(child, SIGKILL);
                      while (::waitpid(child, &status, 0) < 0 && errno == EINTR) {
                      }
                  }
              }
              for (int i = 0; i < 30 && !stop.stop_requested(); ++i)
                  std::this_thread::sleep_for(std::chrono::milliseconds(100));
          }
      }) {}

SourceWorker::~SourceWorker() = default;
