#include "Halide.h"

namespace {

enum class BlurGPUSchedule {
    Inline,          // Fully inlining schedule.
    Cache,           // Schedule caching intermedia result of blur_x.
    Slide,           // Schedule enabling sliding window opt within each
                     // work-item or cuda thread.
    SlideVectorize,  // The same as above plus vectorization per work-item.
};

std::map<std::string, BlurGPUSchedule> blurGPUScheduleEnumMap() {
    return {
        {"inline", BlurGPUSchedule::Inline},
        {"cache", BlurGPUSchedule::Cache},
        {"slide", BlurGPUSchedule::Slide},
        {"slide_vector", BlurGPUSchedule::SlideVectorize},
    };
};

class HalideBlur : public Halide::Generator<HalideBlur> {
public:
    GeneratorParam<BlurGPUSchedule> schedule{
        "schedule",
        BlurGPUSchedule::SlideVectorize,
        blurGPUScheduleEnumMap()};
    GeneratorParam<int> tile_x{"tile_x", 32};  // X tile.
    GeneratorParam<int> tile_y{"tile_y", 8};   // Y tile.

    Input<Buffer<uint16_t>> input{"input", 2};
    Output<Buffer<uint16_t>> blur_y{"blur_y", 2};

    void generate() {
        Func blur_x("blur_x");
        Var x("x"), y("y"), xi("xi"), yi("yi"), xo("xo"), yo("yo"), xii("xii");
        RDom rx(0, 3);
        // The algorithm
        blur_x(x, y) = cast(UInt(16), (cast(UInt(32), (input(x, y) + input(x + 1, y) + input(x + 2, y))) * 21845) >> 16);
        blur_y(x, y) = cast(UInt(16), 0);
        blur_y(x, y) += blur_x(x, y + rx);
        blur_y(x, y) = cast(UInt(16), (cast(UInt(32), blur_y(x, y)) * 21845) >> 16);

        // How to schedule it
        if (get_target().has_gpu_feature()) {
            // GPU schedule.
            switch (schedule) {
            case BlurGPUSchedule::Inline:
                // - Fully inlining.
                blur_y.gpu_tile(x, y, xi, yi, tile_x, tile_y);
                break;
            case BlurGPUSchedule::Cache:
                // - Cache blur_x calculation.
                blur_y.gpu_tile(x, y, xi, yi, tile_x, tile_y);
                blur_x.compute_at(blur_y, x).gpu_threads(x, y);
                break;
            case BlurGPUSchedule::Slide: {
                // - Instead caching blur_x calculation explicitly, the
                //   alternative is to allow each work-item in OpenCL or thread
                //   in CUDA to calculate more rows of blur_y so that temporary
                //   blur_x calculation is re-used implicitly. This achieves
                //   the similar schedule of sliding window.
                Var y_inner("y_inner");
                blur_y
                    .split(y, y, y_inner, tile_y)
                    .reorder(y_inner, x)
                    .unroll(y_inner)
                    .gpu_tile(x, y, xi, yi, tile_x, 1);
                break;
            }
            case BlurGPUSchedule::SlideVectorize: {
                // Vectorization factor.
                int factor = sizeof(int) / sizeof(short);
                Var y_inner("y_inner");
                blur_y.vectorize(x, factor)
                    .split(y, y, y_inner, tile_y)
                    .reorder(y_inner, x)
                    .unroll(y_inner)
                    .gpu_tile(x, y, xi, yi, tile_x, 1);
                break;
            }
            default:
                break;
            }
        } else if (get_target().has_feature(Target::HVX)) {
            // Hexagon schedule.
            // TODO: Try using a schedule like the CPU one below.
            const int vector_size = 128;

            blur_y.compute_root()
                .hexagon()
                .prefetch(input, y, 2)
                .split(y, y, yi, 128)
                .parallel(y)
                .vectorize(x, vector_size * 2);
            blur_x
                .store_at(blur_y, y)
                .compute_at(blur_y, yi)
                .vectorize(x, vector_size);
        } else if (get_target().has_feature(Target::Xtensa)) {
            // const int vector_size = 32;
            // blur_y.split(y, y, yi, 8)
            //     // NOTE(vksnk): parallel is not supported yet.
            //     // .parallel(y)
            //     .vectorize(x, vector_size);
            // blur_x.store_at(blur_y, y).compute_at(blur_y, yi).vectorize(x, vector_size);
#if 0
            blur_y.split(x, xo, xi, 128)
			.split(y, yo, yi, 64)
            .split(xi, xi, xii, 32)
			.vectorize(xii)
		    .reorder(xii,yi,xi,xo,yo);						
			
			blur_x
			// .store_at(blur_y, xi)
            .compute_at(blur_y, xi)
            .vectorize(x, 32);
#else
            blur_y.split(x, xo, xi, 128)
                .split(y, yo, yi, 64)
                .vectorize(xi, 32)
                .reorder(yi, xi, xo, yo);

            blur_x.compute_root().vectorize(x, 32);
            // blur_x
            // // .store_at(blur_y, xi)
            // .compute_at(blur_y, xi)
            // .vectorize(x, 32);

            blur_y.update(0).vectorize(x, 32);
            blur_y.update(1).vectorize(x, 32);
#endif
        } else {
            // CPU schedule.
            // Compute blur_x as needed at each vector of the output.
            // Halide will store blur_x in a circular buffer so its
            // results can be re-used.
            blur_y
                .split(y, y, yi, 32)
                .parallel(y)
                .vectorize(x, 16);
            blur_x
                .store_at(blur_y, y)
                .compute_at(blur_y, x)
                .vectorize(x, 16);
        }

        input.set_host_alignment(64);
        blur_y.set_host_alignment(64);
        input.dim(0)
            .set_min((input.dim(0).min() / 64) * 64)
            .set_extent((input.dim(0).extent() / 64) * 64);

        // input.dim(1)
        //     .set_min((input.dim(1).min() / 4) * 4)
        //     .set_extent((input.dim(1).extent() / 4) * 4);

        input.dim(1).set_stride((input.dim(1).stride() / 64) * 64);

        blur_y.dim(0)
            .set_min((blur_y.dim(0).min() / 64) * 64)
            .set_extent((blur_y.dim(0).extent() / 64) * 64);

        // blur_y.dim(1)
        //     .set_min((blur_y.dim(1).min() / 4) * 4)
        //     .set_extent((blur_y.dim(1).extent() / 4) * 4);

        blur_y.dim(1).set_stride((blur_y.dim(1).stride() / 64) * 64);

        // blur_y.bound(x, 0, 128).bound(y, 0, 128);
    }
};

}  // namespace

HALIDE_REGISTER_GENERATOR(HalideBlur, halide_blur)
