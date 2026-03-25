#include "vgeo_builder.h"

#include <exception>
#include <filesystem>
#include <iostream>
#include <string_view>

namespace {

void print_usage() {
    std::cerr << "Usage: meridian_builder --manifest <path>\n";
}

}  // namespace

int main(int argc, char** argv) {
    if (argc != 3 || std::string_view(argv[1]) != "--manifest") {
        print_usage();
        return 1;
    }

    try {
        const std::filesystem::path manifest_path = argv[2];
        const meridian::BuildManifest manifest = meridian::load_manifest(manifest_path);
        const meridian::VGeoResource resource = meridian::build_resource(manifest);
        meridian::validate_resource(resource);

        const std::filesystem::path output_path = manifest.output_path;
        meridian::write_resource(resource, output_path);
        meridian::write_summary(resource, output_path.string() + ".summary.txt");

        std::cout << "Wrote " << output_path << '\n';
        std::cout << "Wrote " << output_path.string() + ".summary.txt" << '\n';
        return 0;
    } catch (const meridian::BuilderError& error) {
        std::cerr << "Builder error: " << error.what() << '\n';
        return 2;
    } catch (const std::exception& error) {
        std::cerr << "Unexpected error: " << error.what() << '\n';
        return 3;
    }
}
