#ifndef OSMIUM_IO_READER_HPP
#define OSMIUM_IO_READER_HPP

/*

This file is part of Osmium (http://osmcode.org/osmium).

Copyright 2013 Jochen Topf <jochen@topf.org> and others (see README).

Boost Software License - Version 1.0 - August 17th, 2003

Permission is hereby granted, free of charge, to any person or organization
obtaining a copy of the software and accompanying documentation covered by
this license (the "Software") to use, reproduce, display, distribute,
execute, and transmit the Software, and to prepare derivative works of the
Software, and to permit third-parties to whom the Software is furnished to
do so, all subject to the following:

The copyright notices in the Software and this entire statement, including
the above license grant, this restriction and the following disclaimer,
must be included in all copies of the Software, in whole or in part, and
all derivative works of the Software, unless such copies or derivative
works are solely in the form of machine-executable object code generated by
a source language processor.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE, TITLE AND NON-INFRINGEMENT. IN NO EVENT
SHALL THE COPYRIGHT HOLDERS OR ANYONE DISTRIBUTING THE SOFTWARE BE LIABLE
FOR ANY DAMAGES OR OTHER LIABILITY, WHETHER IN CONTRACT, TORT OR OTHERWISE,
ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
DEALINGS IN THE SOFTWARE.

*/

#include <functional>
#include <memory>
#include <string>
#include <system_error>
#include <thread>
#include <utility>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#include <osmium/io/input.hpp>
#include <osmium/io/compression.hpp>
#include <osmium/thread/queue.hpp>
#include <osmium/thread/debug.hpp>

namespace osmium {

    namespace io {

        class InputThread {

            osmium::thread::Queue<std::string>& m_queue;
            const std::string& m_compression;
            const int m_fd;

        public:

            InputThread(osmium::thread::Queue<std::string>& queue, const std::string& compression, int fd) :
                m_queue(queue),
                m_compression(compression),
                m_fd(fd) {
            }

            void operator()() {
                osmium::thread::set_thread_name("_osmium_input");

                std::unique_ptr<osmium::io::Decompressor> decompressor = osmium::io::CompressionFactory::instance().create_decompressor(m_compression, m_fd);

                bool done = false;
                while (!done) {
                    std::string data {decompressor->read()};
                    if (data.empty()) {
                        done = true;
                    }
                    m_queue.push(std::move(data));
                    while (m_queue.size() > 10) {
                        std::this_thread::sleep_for(std::chrono::milliseconds(10));
                    }
                }

                decompressor->close();
            }

        }; // class InputThread

        class Reader {

            osmium::io::File m_file;
            std::unique_ptr<osmium::io::Input> m_input;
            osmium::thread::Queue<std::string> m_input_queue {};
            std::thread m_input_thread;
            osmium::osm_entity::flags m_read_types {osmium::osm_entity::flags::all};
            int m_childpid {0};

            /**
             * Fork and execute the given command in the child.
             * A pipe is created between the child and the parent.
             * The child writes to the pipe, the parent reads from it.
             * This function never returns in the child.
             *
             * @param command Command to execute in the child.
             * @param filename Filename to give to command as argument.
             * @return File descriptor of pipe in the parent.
             * @throws std::system_error if a system call fails.
             */
            int execute(const std::string& command, const std::string& filename) {
                int pipefd[2];
                if (pipe(pipefd) < 0) {
                    throw std::system_error(errno, std::system_category(), "opening pipe failed");
                }
                pid_t pid = fork();
                if (pid < 0) {
                    throw std::system_error(errno, std::system_category(), "fork failed");
                }
                if (pid == 0) { // child
                    // close all file descriptors except one end of the pipe
                    for (int i=0; i < 32; ++i) {
                        if (i != pipefd[1]) {
                            ::close(i);
                        }
                    }
                    if (dup2(pipefd[1], 1) < 0) { // put end of pipe as stdout/stdin
                        exit(1);
                    }

                    ::open("/dev/null", O_RDONLY); // stdin
                    ::open("/dev/null", O_WRONLY); // stderr
                    if (::execlp(command.c_str(), command.c_str(), filename.c_str(), nullptr) < 0) {
                        exit(1);
                    }
                }
                // parent
                m_childpid = pid;
                ::close(pipefd[1]);
                return pipefd[0];
            }

            /**
             * Open File for reading. Handles URLs or normal files. URLs
             * are opened by executing the "curl" program (which must be installed)
             * and reading from its output.
             *
             * @return File descriptor of open file or pipe.
             * @throws std::system_error if a system call fails.
             */
            int open_input_file_or_url(const std::string& filename) {
                std::string protocol = filename.substr(0, filename.find_first_of(':'));
                if (protocol == "http" || protocol == "https" || protocol == "ftp" || protocol == "file") {
                    return execute("curl", filename);
                } else {
                    return osmium::io::detail::open_for_reading(filename);
                }
            }

        public:

            Reader(osmium::io::File file) :
                m_file(std::move(file)),
                m_input(osmium::io::InputFactory::instance().create_input(m_file, m_input_queue)) {
                if (!m_input) {
                    throw std::runtime_error("file type not supported");
                }
                int fd = open_input_file_or_url(m_file.filename());
                m_input_thread = std::thread(InputThread {m_input_queue, m_file.encoding()->compress(), fd});
            }

            Reader(const std::string& filename) :
                Reader(osmium::io::File(filename)) {
            }

            Reader(const Reader&) = delete;
            Reader& operator=(const Reader&) = delete;

            ~Reader() {
                close();
                if (m_input_thread.joinable()) {
                    m_input_thread.join();
                }
            }

            void close() {
                // XXX
//                m_input->close();
                if (m_childpid) {
                    int status;
                    pid_t pid = ::waitpid(m_childpid, &status, 0);
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wold-style-cast"
                    if (pid < 0 || !WIFEXITED(status) || WEXITSTATUS(status) != 0) {
                        throw std::system_error(errno, std::system_category(), "subprocess returned error");
                    }
#pragma GCC diagnostic pop
                    m_childpid = 0;
                }
            }

            osmium::io::Header open(osmium::osm_entity::flags read_types = osmium::osm_entity::flags::all) {
                m_read_types = read_types;
                return m_input->read(read_types);
            }

            osmium::memory::Buffer read() {
                if (m_read_types == osmium::osm_entity::flags::nothing) {
                    // If the caller didn't want anything but the header, it will
                    // always get an empty buffer here.
                    return osmium::memory::Buffer();
                }
                return m_input->next_buffer();
            }

        }; // class Reader

    } // namespace io

} // namespace osmium

#endif // OSMIUM_IO_READER_HPP
