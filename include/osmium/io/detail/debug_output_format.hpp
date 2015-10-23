#ifndef OSMIUM_IO_DETAIL_DEBUG_OUTPUT_FORMAT_HPP
#define OSMIUM_IO_DETAIL_DEBUG_OUTPUT_FORMAT_HPP

/*

This file is part of Osmium (http://osmcode.org/libosmium).

Copyright 2013-2015 Jochen Topf <jochen@topf.org> and others (see README).

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

#include <chrono>
#include <cinttypes>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <future>
#include <iterator>
#include <memory>
#include <ratio>
#include <string>
#include <thread>
#include <utility>

#include <utf8.h>

#include <osmium/io/detail/output_format.hpp>
#include <osmium/io/file_format.hpp>
#include <osmium/memory/buffer.hpp>
#include <osmium/memory/collection.hpp>
#include <osmium/osm/box.hpp>
#include <osmium/osm/changeset.hpp>
#include <osmium/osm/item_type.hpp>
#include <osmium/osm/location.hpp>
#include <osmium/osm/node.hpp>
#include <osmium/osm/object.hpp>
#include <osmium/osm/relation.hpp>
#include <osmium/osm/tag.hpp>
#include <osmium/osm/timestamp.hpp>
#include <osmium/osm/way.hpp>
#include <osmium/thread/pool.hpp>
#include <osmium/util/minmax.hpp>
#include <osmium/visitor.hpp>

namespace osmium {

    namespace io {

        class File;

        namespace detail {

            constexpr const char* color_bold    = "\x1b[1m";
            constexpr const char* color_black   = "\x1b[30m";
            constexpr const char* color_gray    = "\x1b[30;1m";
            constexpr const char* color_red     = "\x1b[31m";
            constexpr const char* color_green   = "\x1b[32m";
            constexpr const char* color_yellow  = "\x1b[33m";
            constexpr const char* color_blue    = "\x1b[34m";
            constexpr const char* color_magenta = "\x1b[35m";
            constexpr const char* color_cyan    = "\x1b[36m";
            constexpr const char* color_white   = "\x1b[37m";
            constexpr const char* color_reset   = "\x1b[0m";

            struct debug_output_options {
                bool add_metadata;
                bool use_color;
            };

            /**
             * Writes out one buffer with OSM data in Debug format.
             */
            class DebugOutputBlock : public OutputBlock {

                debug_output_options m_options;

                void append_encoded_string(const char* data) {
                    const char* end = data + std::strlen(data);

                    while (data != end) {
                        const char* last = data;
                        uint32_t c = utf8::next(data, end);

                        // This is a list of Unicode code points that we let
                        // through instead of escaping them. It is incomplete
                        // and can be extended later.
                        // Generally we don't want to let through any
                        // non-printing characters.
                        if ((0x0020 <= c && c <= 0x0021) ||
                            (0x0023 <= c && c <= 0x003b) ||
                            (0x003d == c) ||
                            (0x003f <= c && c <= 0x007e) ||
                            (0x00a1 <= c && c <= 0x00ac) ||
                            (0x00ae <= c && c <= 0x05ff)) {
                            m_out->append(last, data);
                        } else {
                            write_color(color_red);
                            output_formatted("<U+%04X>", c);
                            write_color(color_blue);
                        }
                    }
                }

                void write_color(const char* color) {
                    if (m_options.use_color) {
                        *m_out += color;
                    }
                }

                void write_string(const char* string) {
                    *m_out += '"';
                    write_color(color_blue);
                    append_encoded_string(string);
                    write_color(color_reset);
                    *m_out += '"';
                }

                void write_object_type(const char* object_type, bool visible = true) {
                    if (visible) {
                        write_color(color_bold);
                    } else {
                        write_color(color_white);
                    }
                    *m_out += object_type;
                    write_color(color_reset);
                    *m_out += ' ';
                }

                void write_fieldname(const char* name) {
                    *m_out += "  ";
                    write_color(color_cyan);
                    *m_out += name;
                    write_color(color_reset);
                    *m_out += ": ";
                }

                void write_comment_field(const char* name) {
                    write_color(color_cyan);
                    *m_out += name;
                    write_color(color_reset);
                    *m_out += ": ";
                }

                void write_counter(int width, int n) {
                    write_color(color_white);
                    output_formatted("    %0*d: ", width, n++);
                    write_color(color_reset);
                }

                void write_error(const char* msg) {
                    write_color(color_red);
                    *m_out += msg;
                    write_color(color_reset);
                }

                void write_meta(const osmium::OSMObject& object) {
                    output_formatted("%" PRId64 "\n", object.id());
                    if (m_options.add_metadata) {
                        write_fieldname("version");
                        output_formatted("  %d", object.version());
                        if (object.visible()) {
                            *m_out += " visible\n";
                        } else {
                            write_error(" deleted\n");
                        }
                        write_fieldname("changeset");
                        output_formatted("%d\n", object.changeset());
                        write_fieldname("timestamp");
                        *m_out += object.timestamp().to_iso();
                        output_formatted(" (%d)\n", object.timestamp());
                        write_fieldname("user");
                        output_formatted("     %d ", object.uid());
                        write_string(object.user());
                        *m_out += '\n';
                    }
                }

                void write_tags(const osmium::TagList& tags, const char* padding="") {
                    if (!tags.empty()) {
                        write_fieldname("tags");
                        *m_out += padding;
                        output_formatted("     %d\n", tags.size());

                        osmium::max_op<size_t> max;
                        for (const auto& tag : tags) {
                            max.update(std::strlen(tag.key()));
                        }
                        for (const auto& tag : tags) {
                            *m_out += "    ";
                            write_string(tag.key());
                            auto spacing = max() - std::strlen(tag.key());
                            while (spacing--) {
                                *m_out += " ";
                            }
                            *m_out += " = ";
                            write_string(tag.value());
                            *m_out += '\n';
                        }
                    }
                }

                void write_location(const osmium::Location& location) {
                    write_fieldname("lon/lat");
                    output_formatted("  %.7f,%.7f", location.lon_without_check(), location.lat_without_check());
                    if (!location.valid()) {
                        write_error(" INVALID LOCATION!");
                    }
                    *m_out += '\n';
                }

                void write_box(const osmium::Box& box) {
                    write_fieldname("box l/b/r/t");
                    if (!box) {
                        write_error("BOX NOT SET!\n");
                        return;
                    }
                    const auto& bl = box.bottom_left();
                    const auto& tr = box.top_right();
                    output_formatted("%.7f,%.7f %.7f,%.7f", bl.lon_without_check(), bl.lat_without_check(), tr.lon_without_check(), tr.lat_without_check());
                    if (!box.valid()) {
                        write_error(" INVALID BOX!");
                    }
                    *m_out += '\n';
                }

            public:

                DebugOutputBlock(osmium::memory::Buffer&& buffer, const debug_output_options& options) :
                    OutputBlock(std::move(buffer)),
                    m_options(options) {
                }

                DebugOutputBlock(const DebugOutputBlock&) = default;
                DebugOutputBlock& operator=(const DebugOutputBlock&) = default;

                DebugOutputBlock(DebugOutputBlock&&) = default;
                DebugOutputBlock& operator=(DebugOutputBlock&&) = default;

                ~DebugOutputBlock() = default;

                std::string operator()() {
                    osmium::apply(m_input_buffer->cbegin(), m_input_buffer->cend(), *this);

                    std::string out;
                    using std::swap;
                    swap(out, *m_out);

                    return out;
                }

                void node(const osmium::Node& node) {
                    write_object_type("node", node.visible());
                    write_meta(node);

                    if (node.visible()) {
                        write_location(node.location());
                    }

                    write_tags(node.tags());

                    *m_out += '\n';
                }

                void way(const osmium::Way& way) {
                    write_object_type("way", way.visible());
                    write_meta(way);
                    write_tags(way.tags());

                    write_fieldname("nodes");

                    output_formatted("    %d", way.nodes().size());
                    if (way.nodes().size() < 2) {
                        write_error(" LESS THAN 2 NODES!\n");
                    } else if (way.nodes().size() > 2000) {
                        write_error(" MORE THAN 2000 NODES!\n");
                    } else if (way.nodes().is_closed()) {
                        *m_out += " (closed)\n";
                    } else {
                        *m_out += " (open)\n";
                    }

                    int width = int(log10(way.nodes().size())) + 1;
                    int n = 0;
                    for (const auto& node_ref : way.nodes()) {
                        write_counter(width, n++);
                        output_formatted("%10" PRId64, node_ref.ref());
                        if (node_ref.location().valid()) {
                            output_formatted(" (%.7f,%.7f)", node_ref.location().lon_without_check(), node_ref.location().lat_without_check());
                        }
                        *m_out += '\n';
                    }

                    *m_out += '\n';
                }

                void relation(const osmium::Relation& relation) {
                    static const char* short_typename[] = { "node", "way ", "rel " };
                    write_object_type("relation", relation.visible());
                    write_meta(relation);
                    write_tags(relation.tags());

                    write_fieldname("members");
                    output_formatted("  %d\n", relation.members().size());

                    int width = int(log10(relation.members().size())) + 1;
                    int n = 0;
                    for (const auto& member : relation.members()) {
                        write_counter(width, n++);
                        *m_out += short_typename[item_type_to_nwr_index(member.type())];
                        output_formatted(" %10" PRId64 " ", member.ref());
                        write_string(member.role());
                        *m_out += '\n';
                    }

                    *m_out += '\n';
                }

                void changeset(const osmium::Changeset& changeset) {
                    write_object_type("changeset");
                    output_formatted("%d\n", changeset.id());

                    write_fieldname("num changes");
                    output_formatted("%d", changeset.num_changes());
                    if (changeset.num_changes() == 0) {
                        write_error(" NO CHANGES!");
                    }
                    *m_out += '\n';

                    write_fieldname("created at");
                    *m_out += ' ';
                    *m_out += changeset.created_at().to_iso();
                    output_formatted(" (%d)\n", changeset.created_at());

                    write_fieldname("closed at");
                    *m_out += "  ";
                    if (changeset.closed()) {
                        *m_out += changeset.closed_at().to_iso();
                        output_formatted(" (%d)\n", changeset.closed_at());
                    } else {
                        write_error("OPEN!\n");
                    }

                    write_fieldname("user");
                    output_formatted("       %d ", changeset.uid());
                    write_string(changeset.user());
                    *m_out += '\n';

                    write_box(changeset.bounds());
                    write_tags(changeset.tags(), "  ");

                    if (changeset.num_comments() > 0) {
                        write_fieldname("comments");
                        output_formatted("   %d\n", changeset.num_comments());

                        int width = int(log10(changeset.num_comments())) + 1;
                        int n = 0;
                        for (const auto& comment : changeset.discussion()) {
                            write_counter(width, n++);

                            write_comment_field("date");
                            *m_out += comment.date().to_iso();
                            output_formatted(" (%d)\n      %*s", comment.date(), width, "");

                            write_comment_field("user");
                            output_formatted("%d ", comment.uid());
                            write_string(comment.user());
                            output_formatted("\n      %*s", width, "");

                            write_comment_field("text");
                            write_string(comment.text());
                            *m_out += '\n';
                        }
                    }

                    *m_out += '\n';
                }

            }; // DebugOutputBlock

            class DebugOutputFormat : public osmium::io::detail::OutputFormat {

                debug_output_options m_options;

            public:

                DebugOutputFormat(const osmium::io::File& file, future_string_queue_type& output_queue) :
                    OutputFormat(file, output_queue),
                    m_options() {
                    m_options.add_metadata = file.get("add_metadata") != "false";
                    m_options.use_color    = file.get("color") == "true";
                }

                DebugOutputFormat(const DebugOutputFormat&) = delete;
                DebugOutputFormat& operator=(const DebugOutputFormat&) = delete;

                void write_buffer(osmium::memory::Buffer&& buffer) override final {
                    m_output_queue.push(osmium::thread::Pool::instance().submit(DebugOutputBlock{std::move(buffer), m_options}));
                }

                void write_fieldname(std::string& out, const char* name) {
                    out += "  ";
                    if (m_options.use_color) {
                        out += color_cyan;
                    }
                    out += name;
                    if (m_options.use_color) {
                        out += color_reset;
                    }
                    out += ": ";
                }

                void write_header(const osmium::io::Header& header) override final {
                    std::string out;

                    if (m_options.use_color) {
                        out += color_bold;
                    }
                    out += "header\n";
                    if (m_options.use_color) {
                        out += color_reset;
                    }

                    write_fieldname(out, "multiple object versions");
                    out += header.has_multiple_object_versions() ? "yes" : "no";
                    out += '\n';
                    write_fieldname(out, "bounding boxes");
                    out += '\n';
                    for (const auto& box : header.boxes()) {
                        out += "    ";
                        box.bottom_left().as_string(std::back_inserter(out), ',');
                        out += " ";
                        box.top_right().as_string(std::back_inserter(out), ',');
                        out += '\n';
                    }
                    write_fieldname(out, "options");
                    out += '\n';
                    for (const auto& opt : header) {
                        out += "    ";
                        out += opt.first;
                        out += " = ";
                        out += opt.second;
                        out += '\n';
                    }
                    out += "\n=============================================\n\n";

                    std::promise<std::string> promise;
                    m_output_queue.push(promise.get_future());
                    promise.set_value(std::move(out));
                }

                void close() override final {
                    std::string out;
                    std::promise<std::string> promise;
                    m_output_queue.push(promise.get_future());
                    promise.set_value(out);
                }

            }; // class DebugOutputFormat

            namespace {

// we want the register_output_format() function to run, setting the variable
// is only a side-effect, it will never be used
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-variable"
                const bool registered_debug_output = osmium::io::detail::OutputFormatFactory::instance().register_output_format(osmium::io::file_format::debug,
                    [](const osmium::io::File& file, future_string_queue_type& output_queue) {
                        return new osmium::io::detail::DebugOutputFormat(file, output_queue);
                });
#pragma GCC diagnostic pop

            } // anonymous namespace

        } // namespace detail

    } // namespace io

} // namespace osmium

#endif // OSMIUM_IO_DETAIL_DEBUG_OUTPUT_FORMAT_HPP
