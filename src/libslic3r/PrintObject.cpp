#include "Exception.hpp"
#include "Print.hpp"
#include "BoundingBox.hpp"
#include "ClipperUtils.hpp"
#include "ElephantFootCompensation.hpp"
#include "Geometry.hpp"
#include "I18N.hpp"
#include "Layer.hpp"
#include "MutablePolygon.hpp"
#include "PrintConfig.hpp"
#include "SupportMaterial.hpp"
#include "SupportSpotsGenerator.hpp"
#include "Support/TreeSupport.hpp"
#include "Surface.hpp"
#include "Slicing.hpp"
#include "Tesselate.hpp"
#include "TriangleMeshSlicer.hpp"
#include "Utils.hpp"
#include "Fill/FillAdaptive.hpp"
#include "Fill/FillLightning.hpp"
#include "Format/STL.hpp"
#include "InternalBridgeDetector.hpp"
#include "TreeSupport.hpp"

#include <float.h>
#include <string_view>
#include <utility>

#include <boost/log/trivial.hpp>

#include <tbb/parallel_for.h>

#include <Shiny/Shiny.h>

using namespace std::literals;

//! macro used to mark string used at localization,
//! return same string
#define L(s) Slic3r::I18N::translate(s)

#ifdef SLIC3R_DEBUG_SLICE_PROCESSING
#define SLIC3R_DEBUG
#endif

// #define SLIC3R_DEBUG

// Make assert active if SLIC3R_DEBUG
#ifdef SLIC3R_DEBUG
    #undef NDEBUG
    #define DEBUG
    #define _DEBUG
    #include "SVG.hpp"
    #undef assert
    #include <cassert>
#endif

namespace Slic3r {

// Constructor is called from the main thread, therefore all Model / ModelObject / ModelIntance data are valid.
PrintObject::PrintObject(Print* print, ModelObject* model_object, const Transform3d& trafo, PrintInstances&& instances) :
    PrintObjectBaseWithState(print, model_object),
    m_trafo(trafo),
    // BBS
    m_tree_support_preview_cache(nullptr)
{
    // Compute centering offet to be applied to our meshes so that we work with smaller coordinates
    // requiring less bits to represent Clipper coordinates.

	// Snug bounding box of a rotated and scaled object by the 1st instantion, without the instance translation applied.
	// All the instances share the transformation matrix with the exception of translation in XY and rotation by Z,
	// therefore a bounding box from 1st instance of a ModelObject is good enough for calculating the object center,
	// snug height and an approximate bounding box in XY.
    BoundingBoxf3  bbox        = model_object->raw_bounding_box();
    Vec3d 		   bbox_center = bbox.center();
	// We may need to rotate the bbox / bbox_center from the original instance to the current instance.
	double z_diff = Geometry::rotation_diff_z(model_object->instances.front()->get_rotation(), instances.front().model_instance->get_rotation());
	if (std::abs(z_diff) > EPSILON) {
		auto z_rot  = Eigen::AngleAxisd(z_diff, Vec3d::UnitZ());
		bbox 		= bbox.transformed(Transform3d(z_rot));
		bbox_center = (z_rot * bbox_center).eval();
	}

    // Center of the transformed mesh (without translation).
    m_center_offset = Point::new_scale(bbox_center.x(), bbox_center.y());
    // Size of the transformed mesh. This bounding may not be snug in XY plane, but it is snug in Z.
    m_size = (bbox.size() * (1. / SCALING_FACTOR)).cast<coord_t>();

    this->set_instances(std::move(instances));
}

PrintObject::~PrintObject()
{
    BOOST_LOG_TRIVIAL(info) << __FUNCTION__ << boost::format(": this=%1%, m_shared_object %2%")%this%m_shared_object;
    if (m_shared_regions && -- m_shared_regions->m_ref_cnt == 0) delete m_shared_regions;
    clear_layers();
    clear_support_layers();
}

PrintBase::ApplyStatus PrintObject::set_instances(PrintInstances &&instances)
{
    for (PrintInstance &i : instances)
    	// Add the center offset, which will be subtracted from the mesh when slicing.
    	i.shift += m_center_offset;
    // Invalidate and set copies.
    PrintBase::ApplyStatus status = PrintBase::APPLY_STATUS_UNCHANGED;
    bool equal_length = instances.size() == m_instances.size();
    bool equal = equal_length && std::equal(instances.begin(), instances.end(), m_instances.begin(),
    	[](const PrintInstance& lhs, const PrintInstance& rhs) { return lhs.model_instance == rhs.model_instance && lhs.shift == rhs.shift; });
    if (! equal) {
        status = PrintBase::APPLY_STATUS_CHANGED;
        if (m_print->invalidate_steps({ psSkirtBrim, psGCodeExport }) ||
            (! equal_length && m_print->invalidate_step(psWipeTower)))
            status = PrintBase::APPLY_STATUS_INVALIDATED;
        m_instances = std::move(instances);
	    for (PrintInstance &i : m_instances)
	    	i.print_object = this;
    }
    return status;
}

std::vector<std::reference_wrapper<const PrintRegion>> PrintObject::all_regions() const
{
    std::vector<std::reference_wrapper<const PrintRegion>> out;
    out.reserve(m_shared_regions->all_regions.size());
    for (const std::unique_ptr<Slic3r::PrintRegion> &region : m_shared_regions->all_regions)
        out.emplace_back(*region.get());
    return out;
}

// 1) Merges typed region slices into stInternal type.
// 2) Increases an "extra perimeters" counter at region slices where needed.
// 3) Generates perimeters, gap fills and fill regions (fill regions of type stInternal).
void PrintObject::make_perimeters()
{
    // prerequisites
    this->slice();

    if (! this->set_started(posPerimeters))
        return;

    m_print->set_status(15, L("Generating walls"));
    BOOST_LOG_TRIVIAL(info) << "Generating walls..." << log_memory_info();

    // Revert the typed slices into untyped slices.
    if (m_typed_slices) {
        for (Layer *layer : m_layers) {
            layer->restore_untyped_slices();
            m_print->throw_if_canceled();
        }
        m_typed_slices = false;
    }

    // compare each layer to the one below, and mark those slices needing
    // one additional inner perimeter, like the top of domed objects-

    // this algorithm makes sure that at least one perimeter is overlapping
    // but we don't generate any extra perimeter if fill density is zero, as they would be floating
    // inside the object - infill_only_where_needed should be the method of choice for printing
    // hollow objects
    for (size_t region_id = 0; region_id < this->num_printing_regions(); ++ region_id) {
        const PrintRegion &region = this->printing_region(region_id);
        //BBS: remove extra_perimeters, always false
        //if (! region.config().extra_perimeters || region.config().wall_loops == 0 || region.config().sparse_infill_density == 0 || this->layer_count() < 2)
            continue;

        BOOST_LOG_TRIVIAL(debug) << "Generating extra perimeters for region " << region_id << " in parallel - start";
        tbb::parallel_for(
            tbb::blocked_range<size_t>(0, m_layers.size() - 1),
            [this, &region, region_id](const tbb::blocked_range<size_t>& range) {
                for (size_t layer_idx = range.begin(); layer_idx < range.end(); ++ layer_idx) {
                    m_print->throw_if_canceled();
                    LayerRegion &layerm                     = *m_layers[layer_idx]->get_region(region_id);
                    const LayerRegion &upper_layerm         = *m_layers[layer_idx+1]->get_region(region_id);
                    const Polygons upper_layerm_polygons    = to_polygons(upper_layerm.slices.surfaces);
                    // Filter upper layer polygons in intersection_ppl by their bounding boxes?
                    // my $upper_layerm_poly_bboxes= [ map $_->bounding_box, @{$upper_layerm_polygons} ];
                    const double total_loop_length      = total_length(upper_layerm_polygons);
                    const coord_t perimeter_spacing     = layerm.flow(frPerimeter).scaled_spacing();
                    const Flow ext_perimeter_flow       = layerm.flow(frExternalPerimeter);
                    const coord_t ext_perimeter_width   = ext_perimeter_flow.scaled_width();
                    const coord_t ext_perimeter_spacing = ext_perimeter_flow.scaled_spacing();

                    for (Surface &slice : layerm.slices.surfaces) {
                        for (;;) {
                            // compute the total thickness of perimeters
                            const coord_t perimeters_thickness = ext_perimeter_width/2 + ext_perimeter_spacing/2
                                + (region.config().wall_loops-1 + slice.extra_perimeters) * perimeter_spacing;
                            // define a critical area where we don't want the upper slice to fall into
                            // (it should either lay over our perimeters or outside this area)
                            const coord_t critical_area_depth = coord_t(perimeter_spacing * 1.5);
                            const Polygons critical_area = diff(
                                offset(slice.expolygon, float(- perimeters_thickness)),
                                offset(slice.expolygon, float(- perimeters_thickness - critical_area_depth))
                            );
                            // check whether a portion of the upper slices falls inside the critical area
                            const Polylines intersection = intersection_pl(to_polylines(upper_layerm_polygons), critical_area);
                            // only add an additional loop if at least 30% of the slice loop would benefit from it
                            if (total_length(intersection) <=  total_loop_length*0.3)
                                break;
                            /*
                            if (0) {
                                require "Slic3r/SVG.pm";
                                Slic3r::SVG::output(
                                    "extra.svg",
                                    no_arrows   => 1,
                                    expolygons  => union_ex($critical_area),
                                    polylines   => [ map $_->split_at_first_point, map $_->p, @{$upper_layerm->slices} ],
                                );
                            }
                            */
                            ++ slice.extra_perimeters;
                        }
                        #ifdef DEBUG
                            if (slice.extra_perimeters > 0)
                                printf("  adding %d more perimeter(s) at layer %zu\n", slice.extra_perimeters, layer_idx);
                        #endif
                    }
                }
            });
        m_print->throw_if_canceled();
        BOOST_LOG_TRIVIAL(debug) << "Generating extra perimeters for region " << region_id << " in parallel - end";
    }

    BOOST_LOG_TRIVIAL(debug) << "Generating perimeters in parallel - start";
    tbb::parallel_for(
        tbb::blocked_range<size_t>(0, m_layers.size()),
        [this](const tbb::blocked_range<size_t>& range) {
            for (size_t layer_idx = range.begin(); layer_idx < range.end(); ++ layer_idx) {
                m_print->throw_if_canceled();
                m_layers[layer_idx]->make_perimeters();
            }
        }
    );
    m_print->throw_if_canceled();
    BOOST_LOG_TRIVIAL(debug) << "Generating perimeters in parallel - end";

    this->set_done(posPerimeters);
}

void PrintObject::prepare_infill()
{
    if (! this->set_started(posPrepareInfill))
        return;
    m_print->set_status(25, L("Generating infill regions"));
    if (m_typed_slices) {
        // To improve robustness of detect_surfaces_type() when reslicing (working with typed slices), see GH issue #7442.
        // The preceding step (perimeter generator) only modifies extra_perimeters and the extra perimeters are only used by discover_vertical_shells()
        // with more than a single region. If this step does not use Surface::extra_perimeters or Surface::extra_perimeters is always zero, it is safe
        // to reset to the untyped slices before re-runnning detect_surfaces_type().
        for (Layer* layer : m_layers) {
            layer->restore_untyped_slices_no_extra_perimeters();
            m_print->throw_if_canceled();
        }
    }

    // This will assign a type (top/bottom/internal) to $layerm->slices.
    // Then the classifcation of $layerm->slices is transfered onto
    // the $layerm->fill_surfaces by clipping $layerm->fill_surfaces
    // by the cummulative area of the previous $layerm->fill_surfaces.
    this->detect_surfaces_type();
    m_print->throw_if_canceled();

    // Decide what surfaces are to be filled.
    // Here the stTop / stBottomBridge / stBottom infill is turned to just stInternal if zero top / bottom infill layers are configured.
    // Also tiny stInternal surfaces are turned to stInternalSolid.
    BOOST_LOG_TRIVIAL(info) << "Preparing fill surfaces..." << log_memory_info();
    for (auto *layer : m_layers)
        for (auto *region : layer->m_regions) {
            region->prepare_fill_surfaces();
            m_print->throw_if_canceled();
        }

    // this will detect bridges and reverse bridges
    // and rearrange top/bottom/internal surfaces
    // It produces enlarged overlapping bridging areas.
    //
    // 1) stBottomBridge / stBottom infill is grown by 3mm and clipped by the total infill area. Bridges are detected. The areas may overlap.
    // 2) stTop is grown by 3mm and clipped by the grown bottom areas. The areas may overlap.
    // 3) Clip the internal surfaces by the grown top/bottom surfaces.
    // 4) Merge surfaces with the same style. This will mostly get rid of the overlaps.
    //FIXME This does not likely merge surfaces, which are supported by a material with different colors, but same properties.
    this->process_external_surfaces();
    m_print->throw_if_canceled();

    // Add solid fills to ensure the shell vertical thickness.
    this->discover_vertical_shells();
    m_print->throw_if_canceled();

    // Debugging output.
#ifdef SLIC3R_DEBUG_SLICE_PROCESSING
    for (size_t region_id = 0; region_id < this->num_printing_regions(); ++ region_id) {
        for (const Layer *layer : m_layers) {
            LayerRegion *layerm = layer->m_regions[region_id];
            layerm->export_region_slices_to_svg_debug("6_discover_vertical_shells-final");
            layerm->export_region_fill_surfaces_to_svg_debug("6_discover_vertical_shells-final");
        } // for each layer
    } // for each region
#endif /* SLIC3R_DEBUG_SLICE_PROCESSING */

    // Detect, which fill surfaces are near external layers.
    // They will be split in internal and internal-solid surfaces.
    // The purpose is to add a configurable number of solid layers to support the TOP surfaces
    // and to add a configurable number of solid layers above the BOTTOM / BOTTOMBRIDGE surfaces
    // to close these surfaces reliably.
    //FIXME Vojtech: Is this a good place to add supporting infills below sloping perimeters?
    this->discover_horizontal_shells();
    m_print->throw_if_canceled();

#ifdef SLIC3R_DEBUG_SLICE_PROCESSING
    for (size_t region_id = 0; region_id < this->num_printing_regions(); ++ region_id) {
        for (const Layer *layer : m_layers) {
            LayerRegion *layerm = layer->m_regions[region_id];
            layerm->export_region_slices_to_svg_debug("7_discover_horizontal_shells-final");
            layerm->export_region_fill_surfaces_to_svg_debug("7_discover_horizontal_shells-final");
        } // for each layer
    } // for each region
#endif /* SLIC3R_DEBUG_SLICE_PROCESSING */

    // Only active if config->infill_only_where_needed. This step trims the sparse infill,
    // so it acts as an internal support. It maintains all other infill types intact.
    // Here the internal surfaces and perimeters have to be supported by the sparse infill.
    //FIXME The surfaces are supported by a sparse infill, but the sparse infill is only as large as the area to support.
    // Likely the sparse infill will not be anchored correctly, so it will not work as intended.
    // Also one wishes the perimeters to be supported by a full infill.
    this->clip_fill_surfaces();
    m_print->throw_if_canceled();

#ifdef SLIC3R_DEBUG_SLICE_PROCESSING
    for (size_t region_id = 0; region_id < this->num_printing_regions(); ++ region_id) {
        for (const Layer *layer : m_layers) {
            LayerRegion *layerm = layer->m_regions[region_id];
            layerm->export_region_slices_to_svg_debug("8_clip_surfaces-final");
            layerm->export_region_fill_surfaces_to_svg_debug("8_clip_surfaces-final");
        } // for each layer
    } // for each region
#endif /* SLIC3R_DEBUG_SLICE_PROCESSING */

    // the following step needs to be done before combination because it may need
    // to remove only half of the combined infill
    this->bridge_over_infill();
    m_print->throw_if_canceled();

    // combine fill surfaces to honor the "infill every N layers" option
    this->combine_infill();
    m_print->throw_if_canceled();

#ifdef SLIC3R_DEBUG_SLICE_PROCESSING
    for (size_t region_id = 0; region_id < this->num_printing_regions(); ++ region_id) {
        for (const Layer *layer : m_layers) {
            LayerRegion *layerm = layer->m_regions[region_id];
            layerm->export_region_slices_to_svg_debug("9_prepare_infill-final");
            layerm->export_region_fill_surfaces_to_svg_debug("9_prepare_infill-final");
        } // for each layer
    } // for each region
    for (const Layer *layer : m_layers) {
        layer->export_region_slices_to_svg_debug("9_prepare_infill-final");
        layer->export_region_fill_surfaces_to_svg_debug("9_prepare_infill-final");
    } // for each layer
#endif /* SLIC3R_DEBUG_SLICE_PROCESSING */

    this->set_done(posPrepareInfill);
}

void PrintObject::infill()
{
    // prerequisites
    this->prepare_infill();

    if (this->set_started(posInfill)) {
        m_print->set_status(35, L("Generating infill toolpath"));

        auto [adaptive_fill_octree, support_fill_octree] = this->prepare_adaptive_infill_data();
        auto lightning_generator = this->prepare_lightning_infill_data();


        BOOST_LOG_TRIVIAL(debug) << "Filling layers in parallel - start";
        tbb::parallel_for(
            tbb::blocked_range<size_t>(0, m_layers.size()),
            [this, &adaptive_fill_octree = adaptive_fill_octree, &support_fill_octree = support_fill_octree, &lightning_generator](const tbb::blocked_range<size_t>& range) {
                for (size_t layer_idx = range.begin(); layer_idx < range.end(); ++ layer_idx) {
                    m_print->throw_if_canceled();
                    m_layers[layer_idx]->make_fills(adaptive_fill_octree.get(), support_fill_octree.get(), lightning_generator.get());
                }
            }
        );
        m_print->throw_if_canceled();
        BOOST_LOG_TRIVIAL(debug) << "Filling layers in parallel - end";
        /*  we could free memory now, but this would make this step not idempotent
        ### $_->fill_surfaces->clear for map @{$_->regions}, @{$object->layers};
        */
        this->set_done(posInfill);
    }
}

void PrintObject::ironing()
{
    if (this->set_started(posIroning)) {
        BOOST_LOG_TRIVIAL(debug) << "Ironing in parallel - start";
        tbb::parallel_for(
            // Ironing starting with layer 0 to support ironing all surfaces.
            tbb::blocked_range<size_t>(0, m_layers.size()),
            [this](const tbb::blocked_range<size_t>& range) {
                for (size_t layer_idx = range.begin(); layer_idx < range.end(); ++ layer_idx) {
                    m_print->throw_if_canceled();
                    m_layers[layer_idx]->make_ironing();
                }
            }
        );
        m_print->throw_if_canceled();
        BOOST_LOG_TRIVIAL(debug) << "Ironing in parallel - end";
        this->set_done(posIroning);
    }
}

// BBS
void PrintObject::clear_overhangs_for_lift()
{
    if (!m_shared_object) {
        for (Layer* l : m_layers)
            l->loverhangs.clear();
    }
}

static const float g_min_overhang_percent_for_lift = 0.3f;

void PrintObject::detect_overhangs_for_lift()
{
    if (this->set_started(posDetectOverhangsForLift)) {
        const double nozzle_diameter = m_print->config().nozzle_diameter.get_at(0);
        const coordf_t line_width = this->config().get_abs_value("line_width", nozzle_diameter);

        const float min_overlap = line_width * g_min_overhang_percent_for_lift;
        size_t num_layers = this->layer_count();
        size_t num_raft_layers = m_slicing_params.raft_layers();

        m_print->set_status(78, L("Detect overhangs for auto-lift"));

        this->clear_overhangs_for_lift();

        tbb::spin_mutex layer_storage_mutex;
        tbb::parallel_for(tbb::blocked_range<size_t>(num_raft_layers + 1, num_layers),
            [this, min_overlap, line_width](const tbb::blocked_range<size_t>& range)
            {
                for (size_t layer_id = range.begin(); layer_id < range.end(); ++layer_id) {
                    Layer& layer = *m_layers[layer_id];
                    Layer& lower_layer = *layer.lower_layer;

                    ExPolygons overhangs = diff_ex(layer.lslices, offset_ex(lower_layer.lslices, scale_(min_overlap)));
                    layer.loverhangs = std::move(offset2_ex(overhangs, -0.1f * scale_(line_width), 0.1f * scale_(line_width)));
                    layer.loverhangs_bbox = get_extents(layer.loverhangs);
                }
            });

        this->set_done(posDetectOverhangsForLift);
    }
}

void PrintObject::generate_support_material()
{
    if (this->set_started(posSupportMaterial)) {
        this->clear_support_layers();

        if ((this->has_support() && m_layers.size() > 1) || (this->has_raft() && ! m_layers.empty())) {
            m_print->set_status(50, L("Generating support"));

            this->_generate_support_material();
            m_print->throw_if_canceled();
        } else if(!m_print->get_no_check_flag()) {
            // BBS: pop a warning if objects have significant amount of overhangs but support material is not enabled
            m_print->set_status(50, L("Checking support necessity"));
            typedef std::chrono::high_resolution_clock clock_;
            typedef std::chrono::duration<double, std::ratio<1> > second_;
            std::chrono::time_point<clock_> t0{ clock_::now() };

            SupportNecessaryType sntype = this->is_support_necessary();

            double duration{ std::chrono::duration_cast<second_>(clock_::now() - t0).count() };
            BOOST_LOG_TRIVIAL(info) << std::fixed << std::setprecision(0) << "is_support_necessary takes " << duration << " secs.";

            if (sntype != NoNeedSupp) {
                std::map<SupportNecessaryType, std::string> reasons = {
                    {SharpTail,L("floating regions")},
                    {Cantilever,L("floating cantilever")},
                    {LargeOverhang,L("large overhangs")} };
                std::string warning_message = format(L("It seems object %s has %s. Please re-orient the object or enable support generation."),
                    this->model_object()->name, reasons[sntype]);
                this->active_step_add_warning(PrintStateBase::WarningLevel::CRITICAL, warning_message, PrintStateBase::SlicingNeedSupportOn);
            }

#if 0
            // Printing without supports. Empty layer means some objects or object parts are levitating,
            // therefore they cannot be printed without supports.
            for (const Layer *layer : m_layers)
                if (layer->empty())
                    throw Slic3r::SlicingError("Levitating objects cannot be printed without supports.");
#endif
        }

        this->set_done(posSupportMaterial);
    }
}

void PrintObject::estimate_curled_extrusions()
{
    if (this->set_started(posEstimateCurledExtrusions)) {
        if ( std::any_of(this->print()->m_print_regions.begin(), this->print()->m_print_regions.end(),
                        [](const PrintRegion *region) { return region->config().enable_overhang_speed.getBool(); })) {

            // Estimate curling of support material and add it to the malformaition lines of each layer
            float support_flow_width = support_material_flow(this, this->config().layer_height).width();
            SupportSpotsGenerator::Params params{this->print()->m_config.filament_type.values,
                                                 float(this->print()->m_config.inner_wall_acceleration.getFloat()),
                                                 this->config().raft_layers.getInt(), this->config().brim_type.value,
                                                 float(this->config().brim_width.getFloat())};
            SupportSpotsGenerator::estimate_malformations(this->layers(), params);
            m_print->throw_if_canceled();
        }
        //this->set_done(posEstimateCurledExtrusions);
    }
}

void PrintObject::simplify_extrusion_path()
{
    if (this->set_started(posSimplifyPath)) {
        m_print->set_status(75, L("Optimizing toolpath"));
        BOOST_LOG_TRIVIAL(debug) << "Simplify extrusion path of object in parallel - start";
        //BBS: infill and walls
        tbb::parallel_for(
            tbb::blocked_range<size_t>(0, m_layers.size()),
            [this](const tbb::blocked_range<size_t>& range) {
                for (size_t layer_idx = range.begin(); layer_idx < range.end(); ++ layer_idx) {
                    m_print->throw_if_canceled();
                    m_layers[layer_idx]->simplify_wall_extrusion_path();
                }
            }
        );
        m_print->throw_if_canceled();
        BOOST_LOG_TRIVIAL(debug) << "Simplify wall extrusion path of object in parallel - end";
        this->set_done(posSimplifyPath);
    }

    if (this->set_started(posSimplifyInfill)) {
        m_print->set_status(75, L("Optimizing toolpath"));
        BOOST_LOG_TRIVIAL(debug) << "Simplify infill extrusion path of object in parallel - start";
        //BBS: infills
        tbb::parallel_for(
            tbb::blocked_range<size_t>(0, m_layers.size()),
            [this](const tbb::blocked_range<size_t>& range) {
                for (size_t layer_idx = range.begin(); layer_idx < range.end(); ++layer_idx) {
                    m_print->throw_if_canceled();
                    m_layers[layer_idx]->simplify_infill_extrusion_path();
                }
            }
        );
        m_print->throw_if_canceled();
        BOOST_LOG_TRIVIAL(debug) << "Simplify infill extrusion path of object in parallel - end";
        this->set_done(posSimplifyInfill);
    }

    if (this->set_started(posSimplifySupportPath)) {
        //BBS: share same progress
        m_print->set_status(75, L("Optimizing toolpath"));
        BOOST_LOG_TRIVIAL(debug) << "Simplify extrusion path of support in parallel - start";
        tbb::parallel_for(
            tbb::blocked_range<size_t>(0, m_support_layers.size()),
            [this](const tbb::blocked_range<size_t>& range) {
                for (size_t layer_idx = range.begin(); layer_idx < range.end(); ++ layer_idx) {
                    m_print->throw_if_canceled();
                    m_support_layers[layer_idx]->simplify_support_extrusion_path();
                }
            }
        );
        m_print->throw_if_canceled();
        BOOST_LOG_TRIVIAL(debug) << "Simplify extrusion path of support in parallel - end";
        this->set_done(posSimplifySupportPath);
    }
}

std::pair<FillAdaptive::OctreePtr, FillAdaptive::OctreePtr> PrintObject::prepare_adaptive_infill_data()
{
    using namespace FillAdaptive;

    auto [adaptive_line_spacing, support_line_spacing] = adaptive_fill_line_spacing(*this);
    if ((adaptive_line_spacing == 0. && support_line_spacing == 0.) || this->layers().empty())
        return std::make_pair(OctreePtr(), OctreePtr());

    indexed_triangle_set mesh = this->model_object()->raw_indexed_triangle_set();
    // Rotate mesh and build octree on it with axis-aligned (standart base) cubes.
    auto to_octree = transform_to_octree().toRotationMatrix();
    its_transform(mesh, to_octree * this->trafo_centered(), true);

    // Triangulate internal bridging surfaces.
    std::vector<std::vector<Vec3d>> overhangs(this->layers().size());
    tbb::parallel_for(
        tbb::blocked_range<int>(0, int(m_layers.size()) - 1),
        [this, &to_octree, &overhangs](const tbb::blocked_range<int> &range) {
            std::vector<Vec3d> &out = overhangs[range.begin()];
            for (int idx_layer = range.begin(); idx_layer < range.end(); ++ idx_layer) {
                m_print->throw_if_canceled();
                const Layer *layer = this->layers()[idx_layer];
                for (const LayerRegion *layerm : layer->regions())
                    for (const Surface &surface : layerm->fill_surfaces.surfaces)
                        if (surface.surface_type == stInternalBridge)
                            append(out, triangulate_expolygon_3d(surface.expolygon, layer->bottom_z()));
            }
            for (Vec3d &p : out)
                p = (to_octree * p).eval();
        });
    // and gather them.
    for (size_t i = 1; i < overhangs.size(); ++ i)
        append(overhangs.front(), std::move(overhangs[i]));

    return std::make_pair(
        adaptive_line_spacing ? build_octree(mesh, overhangs.front(), adaptive_line_spacing, false) : OctreePtr(),
        support_line_spacing  ? build_octree(mesh, overhangs.front(), support_line_spacing, true) : OctreePtr());
}

FillLightning::GeneratorPtr PrintObject::prepare_lightning_infill_data()
{
    bool has_lightning_infill = false;
    for (size_t region_id = 0; region_id < this->num_printing_regions(); ++region_id)
        if (const PrintRegionConfig& config = this->printing_region(region_id).config(); config.sparse_infill_density > 0 && config.sparse_infill_pattern == ipLightning) {
            has_lightning_infill = true;
            break;
        }

    return has_lightning_infill ? FillLightning::build_generator(std::as_const(*this), [this]() -> void { this->throw_if_canceled(); }) : FillLightning::GeneratorPtr();
}

void PrintObject::clear_layers()
{
    if (!m_shared_object) {
        for (Layer *l : m_layers)
            delete l;
        m_layers.clear();
    }
}

Layer* PrintObject::add_layer(int id, coordf_t height, coordf_t print_z, coordf_t slice_z)
{
    m_layers.emplace_back(new Layer(id, this, height, print_z, slice_z));
    return m_layers.back();
}

const SupportLayer* PrintObject::get_support_layer_at_printz(coordf_t print_z, coordf_t epsilon) const
{
    coordf_t limit = print_z - epsilon;
    auto it = Slic3r::lower_bound_by_predicate(m_support_layers.begin(), m_support_layers.end(), [limit](const SupportLayer* layer) { return layer->print_z < limit; });
    return (it == m_support_layers.end() || (*it)->print_z > print_z + epsilon) ? nullptr : *it;
}

SupportLayer* PrintObject::get_support_layer_at_printz(coordf_t print_z, coordf_t epsilon)
{
    return const_cast<SupportLayer*>(std::as_const(*this).get_support_layer_at_printz(print_z, epsilon));
}

void PrintObject::clear_support_layers()
{
    if (!m_shared_object) {
        for (SupportLayer* l : m_support_layers)
            delete l;
        m_support_layers.clear();
        for (auto l : m_layers) {
            l->sharp_tails.clear();
            l->sharp_tails_height.clear();
            l->cantilevers.clear();
        }
    }
}

std::shared_ptr<TreeSupportData> PrintObject::alloc_tree_support_preview_cache()
{
    if (!m_tree_support_preview_cache) {
        const coordf_t layer_height = m_config.layer_height.value;
        const coordf_t xy_distance = m_config.support_object_xy_distance.value;
        const double angle = m_config.tree_support_branch_angle.value * M_PI / 180.;
        const coordf_t max_move_distance
            = (angle < M_PI / 2) ? (coordf_t)(tan(angle) * layer_height) : std::numeric_limits<coordf_t>::max();
        const coordf_t radius_sample_resolution = g_config_tree_support_collision_resolution;

        m_tree_support_preview_cache = std::make_shared<TreeSupportData>(*this, xy_distance, max_move_distance, radius_sample_resolution);
    }

    return m_tree_support_preview_cache;
}

SupportLayer* PrintObject::add_tree_support_layer(int id, coordf_t height, coordf_t print_z, coordf_t slice_z)
{
    m_support_layers.emplace_back(new SupportLayer(id, 0, this, height, print_z, slice_z));
    m_support_layers.back()->support_type = stInnerTree;
    return m_support_layers.back();
}

SupportLayer* PrintObject::add_support_layer(int id, int interface_id, coordf_t height, coordf_t print_z)
{
    m_support_layers.emplace_back(new SupportLayer(id, interface_id, this, height, print_z, -1));
    return m_support_layers.back();
}

SupportLayerPtrs::iterator PrintObject::insert_support_layer(SupportLayerPtrs::iterator pos, size_t id, size_t interface_id, coordf_t height, coordf_t print_z, coordf_t slice_z)
{
    return m_support_layers.insert(pos, new SupportLayer(id, interface_id, this, height, print_z, slice_z));
}

// Called by Print::apply().
// This method only accepts PrintObjectConfig and PrintRegionConfig option keys.
bool PrintObject::invalidate_state_by_config_options(
    const ConfigOptionResolver &old_config, const ConfigOptionResolver &new_config, const std::vector<t_config_option_key> &opt_keys)
{
    if (opt_keys.empty())
        return false;

    std::vector<PrintObjectStep> steps;
    bool invalidated = false;
    for (const t_config_option_key &opt_key : opt_keys) {
        if (   opt_key == "brim_width"
            || opt_key == "brim_object_gap"
            || opt_key == "brim_type"
            || opt_key == "brim_ears_max_angle"
            || opt_key == "brim_ears_detection_length"
            // BBS: brim generation depends on printing speed
            || opt_key == "outer_wall_speed"
            || opt_key == "small_perimeter_speed"
            || opt_key == "small_perimeter_threshold"
            || opt_key == "sparse_infill_speed"
            || opt_key == "inner_wall_speed"
            || opt_key == "support_speed"
            || opt_key == "internal_solid_infill_speed"
            || opt_key == "top_surface_speed") {
            // Brim is printed below supports, support invalidates brim and skirt.
            steps.emplace_back(posSupportMaterial);
            if (opt_key == "brim_type") {
                const auto* old_brim_type = old_config.option<ConfigOptionEnum<BrimType>>(opt_key);
                const auto* new_brim_type = new_config.option<ConfigOptionEnum<BrimType>>(opt_key);
                //BBS: When switch to manual brim, the object must have brim, then re-generate perimeter
                //to make the wall order of first layer to be outer-first
                if (old_brim_type->value == btOuterOnly || new_brim_type->value == btOuterOnly)
                    steps.emplace_back(posPerimeters);
            }
        } else if (
               opt_key == "wall_loops"
            || opt_key == "only_one_wall_top"
            || opt_key == "only_one_wall_first_layer"
            || opt_key == "extra_perimeters_on_overhangs"
            || opt_key == "initial_layer_line_width"
            || opt_key == "inner_wall_line_width"
            || opt_key == "infill_wall_overlap"
            || opt_key == "seam_gap"
            || opt_key == "role_based_wipe_speed"
            || opt_key == "wipe_on_loops"
            || opt_key == "wipe_speed") {
            steps.emplace_back(posPerimeters);
        } else if (opt_key == "gap_infill_speed"
            || opt_key == "filter_out_gap_fill" ) {
            // Return true if gap-fill speed has changed from zero value to non-zero or from non-zero value to zero.
            auto is_gap_fill_changed_state_due_to_speed = [&opt_key, &old_config, &new_config]() -> bool {
                if (opt_key == "gap_infill_speed") {
                    const auto *old_gap_fill_speed = old_config.option<ConfigOptionFloat>(opt_key);
                    const auto *new_gap_fill_speed = new_config.option<ConfigOptionFloat>(opt_key);
                    assert(old_gap_fill_speed && new_gap_fill_speed);
                    return (old_gap_fill_speed->value > 0.f && new_gap_fill_speed->value == 0.f) ||
                           (old_gap_fill_speed->value == 0.f && new_gap_fill_speed->value > 0.f);
                }
                return false;
            };

            // Filtering of unprintable regions in multi-material segmentation depends on if gap-fill is enabled or not.
            // So step posSlice is invalidated when gap-fill was enabled/disabled by option "filter_out_gap_fill" or by
            // changing "gap_infill_speed" to force recomputation of the multi-material segmentation.
            if (this->is_mm_painted() && (opt_key == "filter_out_gap_fill" && (opt_key == "gap_infill_speed" && is_gap_fill_changed_state_due_to_speed())))
                steps.emplace_back(posSlice);
            steps.emplace_back(posPerimeters);
        } else if (
               opt_key == "layer_height"
            || opt_key == "raft_layers"
            || opt_key == "raft_contact_distance"
            || opt_key == "slice_closing_radius"
            || opt_key == "slicing_mode"
            || opt_key == "slowdown_for_curled_perimeters"
            || opt_key == "make_overhang_printable"
            || opt_key == "make_overhang_printable_angle"
            || opt_key == "make_overhang_printable_hole_size") {
            steps.emplace_back(posSlice);
		} else if (
               opt_key == "elefant_foot_compensation"
            || opt_key == "support_top_z_distance"
            || opt_key == "support_bottom_z_distance"
            || opt_key == "xy_hole_compensation"
            || opt_key == "xy_contour_compensation"
            //BBS: [Arthur] the following params affect bottomBridge surface type detection
            || opt_key == "support_type"
            || opt_key == "bridge_no_support"
            || opt_key == "max_bridge_length"
            || opt_key == "support_interface_top_layers"
            || opt_key == "support_critical_regions_only"
            ) {
            steps.emplace_back(posSlice);
        } else if (opt_key == "enable_support") {
            steps.emplace_back(posSupportMaterial);
            if (m_config.support_top_z_distance == 0.) {
            	// Enabling / disabling supports while soluble support interface is enabled.
            	// This changes the bridging logic (bridging enabled without supports, disabled with supports).
            	// Reset everything.
            	// See GH #1482 for details.
	            steps.emplace_back(posSlice);
	        }
        } else if (
        	   opt_key == "support_type"
            || opt_key == "support_angle"
            || opt_key == "support_on_build_plate_only"
            || opt_key == "support_critical_regions_only"
            || opt_key == "support_remove_small_overhang"
            || opt_key == "enforce_support_layers"
            || opt_key == "support_filament"
            || opt_key == "support_line_width"
            || opt_key == "support_interface_top_layers"
            || opt_key == "support_interface_bottom_layers"
            || opt_key == "support_interface_pattern"
            || opt_key == "support_interface_loop_pattern"
            || opt_key == "support_interface_filament"
            || opt_key == "support_interface_spacing"
            || opt_key == "support_bottom_interface_spacing" //BBS
            || opt_key == "support_base_pattern"
            || opt_key == "support_style"
            || opt_key == "support_object_xy_distance"
            || opt_key == "support_base_pattern_spacing"
            || opt_key == "support_expansion"
            //|| opt_key == "independent_support_layer_height" // BBS
            || opt_key == "support_threshold_angle"
            || opt_key == "raft_expansion"
            || opt_key == "raft_first_layer_density"
            || opt_key == "raft_first_layer_expansion"
            || opt_key == "bridge_no_support"
            || opt_key == "max_bridge_length"
            || opt_key == "initial_layer_line_width"
            || opt_key == "tree_support_adaptive_layer_height"
            || opt_key == "tree_support_auto_brim"
            || opt_key == "tree_support_brim_width"
            || opt_key == "tree_support_top_rate"
            || opt_key == "tree_support_branch_distance"
            || opt_key == "tree_support_branch_distance_organic"
            || opt_key == "tree_support_tip_diameter"
            || opt_key == "tree_support_branch_diameter"
            || opt_key == "tree_support_branch_diameter_organic"
            || opt_key == "tree_support_branch_diameter_angle"
            || opt_key == "tree_support_branch_diameter_double_wall"
            || opt_key == "tree_support_branch_angle"
            || opt_key == "tree_support_branch_angle_organic"
            || opt_key == "tree_support_angle_slow"
            || opt_key == "tree_support_wall_count") {
            steps.emplace_back(posSupportMaterial);
        } else if (
               opt_key == "bottom_shell_layers"
            || opt_key == "top_shell_layers") {
            
            steps.emplace_back(posPrepareInfill);

            const auto *old_shell_layers = old_config.option<ConfigOptionInt>(opt_key);
            const auto *new_shell_layers = new_config.option<ConfigOptionInt>(opt_key);
            assert(old_shell_layers && new_shell_layers);

            bool value_changed = (old_shell_layers->value == 0 && new_shell_layers->value > 0) ||
                                 (old_shell_layers->value > 0 && new_shell_layers->value == 0);

            if (value_changed && this->object_extruders().size() > 1) {
                steps.emplace_back(posSlice);
            }               
            else if (m_print->config().spiral_mode && opt_key == "bottom_shell_layers") {
                // Changing the number of bottom layers when a spiral vase is enabled requires re-slicing the object again.
                // Otherwise, holes in the bottom layers could be filled, as is reported in GH #5528.
                steps.emplace_back(posSlice);
            }
        } else if (
               opt_key == "interface_shells"
            || opt_key == "infill_combination"
            || opt_key == "bottom_shell_thickness"
            || opt_key == "top_shell_thickness"
            || opt_key == "minimum_sparse_infill_area"
            || opt_key == "sparse_infill_filament"
            || opt_key == "solid_infill_filament"
            || opt_key == "sparse_infill_line_width"
            || opt_key == "infill_direction"
            || opt_key == "ensure_vertical_shell_thickness"
            || opt_key == "bridge_angle"
            //BBS
            || opt_key == "internal_bridge_support_thickness"
            || opt_key == "bridge_density") {
            steps.emplace_back(posPrepareInfill);
        } else if (
               opt_key == "top_surface_pattern"
            || opt_key == "bottom_surface_pattern"
            || opt_key == "internal_solid_infill_pattern"
            || opt_key == "external_fill_link_max_length"
            || opt_key == "sparse_infill_pattern"
            || opt_key == "infill_anchor"
            || opt_key == "infill_anchor_max"
            || opt_key == "top_surface_line_width"
            || opt_key == "initial_layer_line_width") {
            steps.emplace_back(posInfill);
        } else if (opt_key == "sparse_infill_pattern") {
            steps.emplace_back(posInfill);
            const auto *old_fill_pattern = old_config.option<ConfigOptionEnum<InfillPattern>>(opt_key);
            const auto *new_fill_pattern = new_config.option<ConfigOptionEnum<InfillPattern>>(opt_key);
            assert(old_fill_pattern && new_fill_pattern);
            // We need to recalculate infill surfaces when infill_only_where_needed is enabled, and we are switching from
            // the Lightning infill to another infill or vice versa.
            if (PrintObject::infill_only_where_needed && (new_fill_pattern->value == ipLightning || old_fill_pattern->value == ipLightning))
                steps.emplace_back(posPrepareInfill);
        } else if (opt_key == "sparse_infill_density") {
            // One likely wants to reslice only when switching between zero infill to simulate boolean difference (subtracting volumes),
            // normal infill and 100% (solid) infill.
            const auto *old_density = old_config.option<ConfigOptionPercent>(opt_key);
            const auto *new_density = new_config.option<ConfigOptionPercent>(opt_key);
            assert(old_density && new_density);
            //FIXME Vojtech is not quite sure about the 100% here, maybe it is not needed.
            if (is_approx(old_density->value, 0.) || is_approx(old_density->value, 100.) ||
                is_approx(new_density->value, 0.) || is_approx(new_density->value, 100.))
                steps.emplace_back(posPerimeters);
            steps.emplace_back(posPrepareInfill);
        } else if (opt_key == "internal_solid_infill_line_width") {
            // This value is used for calculating perimeter - infill overlap, thus perimeters need to be recalculated.
            steps.emplace_back(posPerimeters);
            steps.emplace_back(posPrepareInfill);
        } else if (
               opt_key == "outer_wall_line_width"
            || opt_key == "wall_filament"
            || opt_key == "fuzzy_skin"
            || opt_key == "fuzzy_skin_thickness"
            || opt_key == "fuzzy_skin_point_distance"
            || opt_key == "detect_overhang_wall"
            //BBS
            || opt_key == "enable_overhang_speed"
            || opt_key == "detect_thin_wall"
            || opt_key == "precise_outer_wall"
            || opt_key == "overhang_speed_classic") {
            steps.emplace_back(posPerimeters);
            steps.emplace_back(posSupportMaterial);
        } else if (opt_key == "bridge_flow") {
            if (m_config.support_top_z_distance > 0.) {
            	// Only invalidate due to bridging if bridging is enabled.
            	// If later "support_top_z_distance" is modified, the complete PrintObject is invalidated anyway.
            	steps.emplace_back(posPerimeters);
            	steps.emplace_back(posInfill);
	            steps.emplace_back(posSupportMaterial);
	        }
        } else if (
                opt_key == "wall_generator"
            || opt_key == "wall_transition_length"
            || opt_key == "wall_transition_filter_deviation"
            || opt_key == "wall_transition_angle"
            || opt_key == "wall_distribution_count"
            || opt_key == "min_feature_size"
            || opt_key == "min_bead_width") {
            steps.emplace_back(posSlice);
        } else if (
               opt_key == "seam_position"
            || opt_key == "support_speed"
            || opt_key == "support_interface_speed"
            || opt_key == "overhang_1_4_speed"
            || opt_key == "overhang_2_4_speed"
            || opt_key == "overhang_3_4_speed"
            || opt_key == "overhang_4_4_speed"
            || opt_key == "bridge_speed"
            || opt_key == "internal_bridge_speed"
            || opt_key == "outer_wall_speed"
            || opt_key == "small_perimeter_speed"
            || opt_key == "small_perimeter_threshold"
            || opt_key == "sparse_infill_speed"
            || opt_key == "inner_wall_speed"
            || opt_key == "internal_solid_infill_speed"
            || opt_key == "top_surface_speed") {
            invalidated |= m_print->invalidate_step(psGCodeExport);
        } else if (
               opt_key == "flush_into_infill"
            || opt_key == "flush_into_objects"
            || opt_key == "flush_into_support") {
            invalidated |= m_print->invalidate_step(psWipeTower);
            invalidated |= m_print->invalidate_step(psGCodeExport);
        } else {
            // for legacy, if we can't handle this option let's invalidate all steps
            this->invalidate_all_steps();
            invalidated = true;
        }
    }

    sort_remove_duplicates(steps);
    for (PrintObjectStep step : steps)
        invalidated |= this->invalidate_step(step);
    return invalidated;
}

bool PrintObject::invalidate_step(PrintObjectStep step)
{
	bool invalidated = Inherited::invalidate_step(step);

    // propagate to dependent steps
    if (step == posPerimeters) {
		invalidated |= this->invalidate_steps({ posPrepareInfill, posInfill, posIroning, posSimplifyPath, posSimplifyInfill });
        invalidated |= m_print->invalidate_steps({ psSkirtBrim });
    } else if (step == posPrepareInfill) {
        invalidated |= this->invalidate_steps({ posInfill, posIroning, posSimplifyPath, posSimplifyInfill });
    } else if (step == posInfill) {
        invalidated |= this->invalidate_steps({ posIroning, posSimplifyInfill });
        invalidated |= m_print->invalidate_steps({ psSkirtBrim });
    } else if (step == posSlice) {
		invalidated |= this->invalidate_steps({ posPerimeters, posPrepareInfill, posInfill, posIroning, posSupportMaterial, posSimplifyPath, posSimplifyInfill });
        invalidated |= m_print->invalidate_steps({ psSkirtBrim });
        m_slicing_params.valid = false;
    } else if (step == posSupportMaterial) {
        invalidated |= this->invalidate_steps({ posSimplifySupportPath });
        invalidated |= m_print->invalidate_steps({ psSkirtBrim });
        m_slicing_params.valid = false;
    }

    // Wipe tower depends on the ordering of extruders, which in turn depends on everything.
    // It also decides about what the flush_into_infill / wipe_into_object / flush_into_support features will do,
    // and that too depends on many of the settings.
    invalidated |= m_print->invalidate_step(psWipeTower);
    // Invalidate G-code export in any case.
    invalidated |= m_print->invalidate_step(psGCodeExport);
    return invalidated;
}

bool PrintObject::invalidate_all_steps()
{
	// First call the "invalidate" functions, which may cancel background processing.
    bool result = Inherited::invalidate_all_steps() | m_print->invalidate_all_steps();
	// Then reset some of the depending values.
	m_slicing_params.valid = false;
	return result;
}

// This function analyzes slices of a region (SurfaceCollection slices).
// Each region slice (instance of Surface) is analyzed, whether it is supported or whether it is the top surface.
// Initially all slices are of type stInternal.
// Slices are compared against the top / bottom slices and regions and classified to the following groups:
// stTop          - Part of a region, which is not covered by any upper layer. This surface will be filled with a top solid infill.
// stBottomBridge - Part of a region, which is not fully supported, but it hangs in the air, or it hangs losely on a support or a raft.
// stBottom       - Part of a region, which is not supported by the same region, but it is supported either by another region, or by a soluble interface layer.
// stInternal     - Part of a region, which is supported by the same region type.
// If a part of a region is of stBottom and stTop, the stBottom wins.
void PrintObject::detect_surfaces_type()
{
    BOOST_LOG_TRIVIAL(info) << "Detecting solid surfaces..." << log_memory_info();

    // Interface shells: the intersecting parts are treated as self standing objects supporting each other.
    // Each of the objects will have a full number of top / bottom layers, even if these top / bottom layers
    // are completely hidden inside a collective body of intersecting parts.
    // This is useful if one of the parts is to be dissolved, or if it is transparent and the internal shells
    // should be visible.
    bool spiral_mode      = this->print()->config().spiral_mode.value;
    bool interface_shells = ! spiral_mode && m_config.interface_shells.value;
    size_t num_layers     = spiral_mode ? std::min(size_t(this->printing_region(0).config().bottom_shell_layers), m_layers.size()) : m_layers.size();

    for (size_t region_id = 0; region_id < this->num_printing_regions(); ++ region_id) {
        BOOST_LOG_TRIVIAL(debug) << "Detecting solid surfaces for region " << region_id << " in parallel - start";
#ifdef SLIC3R_DEBUG_SLICE_PROCESSING
        for (Layer *layer : m_layers)
            layer->m_regions[region_id]->export_region_fill_surfaces_to_svg_debug("1_detect_surfaces_type-initial");
#endif /* SLIC3R_DEBUG_SLICE_PROCESSING */

        // If interface shells are allowed, the region->surfaces cannot be overwritten as they may be used by other threads.
        // Cache the result of the following parallel_loop.
        std::vector<Surfaces> surfaces_new;
        if (interface_shells)
            surfaces_new.assign(num_layers, Surfaces());

        tbb::parallel_for(
            tbb::blocked_range<size_t>(0,
            	spiral_mode ?
            		// In spiral vase mode, reserve the last layer for the top surface if more than 1 layer is planned for the vase bottom.
            		((num_layers > 1) ? num_layers - 1 : num_layers) :
            		// In non-spiral vase mode, go over all layers.
            		m_layers.size()),
            [this, region_id, interface_shells, &surfaces_new](const tbb::blocked_range<size_t>& range) {
                // If we have soluble support material, don't bridge. The overhang will be squished against a soluble layer separating
                // the support from the print.
                // BBS: the above logic only applys for normal(auto) support. Complete logic:
                // 1. has support, top z distance=0 (soluble material), auto support
                // 2. for normal(auto), bridge_no_support is off
                // 3. for tree(auto), interface top layers=0, max bridge length=0, support_critical_regions_only=false (only in this way the bridge is fully supported)
                bool bottom_is_fully_supported = this->has_support() && m_config.support_top_z_distance.value == 0 && is_auto(m_config.support_type.value);
                if (m_config.support_type.value == stNormalAuto)
                    bottom_is_fully_supported &= !m_config.bridge_no_support.value;
                else if (m_config.support_type.value == stTreeAuto) {
                    bottom_is_fully_supported &= (m_config.support_interface_top_layers.value > 0 && m_config.max_bridge_length.value == 0 && m_config.support_critical_regions_only.value==false);
                }
                SurfaceType surface_type_bottom_other = bottom_is_fully_supported ? stBottom : stBottomBridge;
                for (size_t idx_layer = range.begin(); idx_layer < range.end(); ++ idx_layer) {
                    m_print->throw_if_canceled();
                    // BOOST_LOG_TRIVIAL(trace) << "Detecting solid surfaces for region " << region_id << " and layer " << layer->print_z;
                    Layer       *layer  = m_layers[idx_layer];
                    LayerRegion *layerm = layer->m_regions[region_id];
                    // comparison happens against the *full* slices (considering all regions)
                    // unless internal shells are requested
                    Layer       *upper_layer = (idx_layer + 1 < this->layer_count()) ? m_layers[idx_layer + 1] : nullptr;
                    Layer       *lower_layer = (idx_layer > 0) ? m_layers[idx_layer - 1] : nullptr;
                    // collapse very narrow parts (using the safety offset in the diff is not enough)
                    float        offset = layerm->flow(frExternalPerimeter).scaled_width() / 10.f;

                    // find top surfaces (difference between current surfaces
                    // of current layer and upper one)
                    Surfaces top;
                    if (upper_layer) {
                        ExPolygons upper_slices = interface_shells ?
                            diff_ex(layerm->slices.surfaces, upper_layer->m_regions[region_id]->slices.surfaces, ApplySafetyOffset::Yes) :
                            diff_ex(layerm->slices.surfaces, upper_layer->lslices, ApplySafetyOffset::Yes);
                        surfaces_append(top, opening_ex(upper_slices, offset), stTop);
                    } else {
                        // if no upper layer, all surfaces of this one are solid
                        // we clone surfaces because we're going to clear the slices collection
                        top = layerm->slices.surfaces;
                        for (Surface &surface : top)
                            surface.surface_type = stTop;
                    }

                    // Find bottom surfaces (difference between current surfaces of current layer and lower one).
                    Surfaces bottom;
                    if (lower_layer) {
#if 0
                        //FIXME Why is this branch failing t\multi.t ?
                        Polygons lower_slices = interface_shells ?
                            to_polygons(lower_layer->get_region(region_id)->slices.surfaces) :
                            to_polygons(lower_layer->slices);
                        surfaces_append(bottom,
                            opening_ex(diff(layerm->slices.surfaces, lower_slices, true), offset),
                            surface_type_bottom_other);
#else
                        // Any surface lying on the void is a true bottom bridge (an overhang)
                        surfaces_append(
                            bottom,
                            opening_ex(
                                diff_ex(layerm->slices.surfaces, lower_layer->lslices, ApplySafetyOffset::Yes),
                                offset),
                            surface_type_bottom_other);
                        // if user requested internal shells, we need to identify surfaces
                        // lying on other slices not belonging to this region
                        if (interface_shells) {
                            // non-bridging bottom surfaces: any part of this layer lying
                            // on something else, excluding those lying on our own region
                            surfaces_append(
                                bottom,
                                opening_ex(
                                    diff_ex(
                                        intersection(layerm->slices.surfaces, lower_layer->lslices), // supported
                                        lower_layer->m_regions[region_id]->slices.surfaces,
                                        ApplySafetyOffset::Yes),
                                    offset),
                                stBottom);
                        }
#endif
                    } else {
                        // if no lower layer, all surfaces of this one are solid
                        // we clone surfaces because we're going to clear the slices collection
                        bottom = layerm->slices.surfaces;
                        for (Surface &surface : bottom)
                            surface.surface_type = stBottom;
                    }

                    // now, if the object contained a thin membrane, we could have overlapping bottom
                    // and top surfaces; let's do an intersection to discover them and consider them
                    // as bottom surfaces (to allow for bridge detection)
                    if (! top.empty() && ! bottom.empty()) {
        //                Polygons overlapping = intersection(to_polygons(top), to_polygons(bottom));
        //                Slic3r::debugf "  layer %d contains %d membrane(s)\n", $layerm->layer->id, scalar(@$overlapping)
        //                    if $Slic3r::debug;
                        Polygons top_polygons = to_polygons(std::move(top));
                        top.clear();
                        surfaces_append(top, diff_ex(top_polygons, bottom), stTop);
                    }

        #ifdef SLIC3R_DEBUG_SLICE_PROCESSING
                    {
                        static int iRun = 0;
                        std::vector<std::pair<Slic3r::ExPolygons, SVG::ExPolygonAttributes>> expolygons_with_attributes;
                        expolygons_with_attributes.emplace_back(std::make_pair(union_ex(top),                           SVG::ExPolygonAttributes("green")));
                        expolygons_with_attributes.emplace_back(std::make_pair(union_ex(bottom),                        SVG::ExPolygonAttributes("brown")));
                        expolygons_with_attributes.emplace_back(std::make_pair(to_expolygons(layerm->slices.surfaces),  SVG::ExPolygonAttributes("black")));
                        SVG::export_expolygons(debug_out_path("1_detect_surfaces_type_%d_region%d-layer_%f.svg", iRun ++, region_id, layer->print_z).c_str(), expolygons_with_attributes);
                    }
        #endif /* SLIC3R_DEBUG_SLICE_PROCESSING */

                    // save surfaces to layer
                    Surfaces &surfaces_out = interface_shells ? surfaces_new[idx_layer] : layerm->slices.surfaces;
                    Surfaces  surfaces_backup;
                    if (! interface_shells) {
                        surfaces_backup = std::move(surfaces_out);
                        surfaces_out.clear();
                    }
                    const Surfaces &surfaces_prev = interface_shells ? layerm->slices.surfaces : surfaces_backup;

                    // find internal surfaces (difference between top/bottom surfaces and others)
                    {
                        Polygons topbottom = to_polygons(top);
                        polygons_append(topbottom, to_polygons(bottom));
                        surfaces_append(surfaces_out, diff_ex(surfaces_prev, topbottom), stInternal);
                    }

                    surfaces_append(surfaces_out, std::move(top));
                    surfaces_append(surfaces_out, std::move(bottom));

        //            Slic3r::debugf "  layer %d has %d bottom, %d top and %d internal surfaces\n",
        //                $layerm->layer->id, scalar(@bottom), scalar(@top), scalar(@internal) if $Slic3r::debug;

        #ifdef SLIC3R_DEBUG_SLICE_PROCESSING
                    layerm->export_region_slices_to_svg_debug("detect_surfaces_type-final");
        #endif /* SLIC3R_DEBUG_SLICE_PROCESSING */
                }
            }
        ); // for each layer of a region
        m_print->throw_if_canceled();

        if (interface_shells) {
            // Move surfaces_new to layerm->slices.surfaces
            for (size_t idx_layer = 0; idx_layer < num_layers; ++ idx_layer)
                m_layers[idx_layer]->m_regions[region_id]->slices.surfaces = std::move(surfaces_new[idx_layer]);
        }

        if (spiral_mode) {
        	if (num_layers > 1)
	        	// Turn the last bottom layer infill to a top infill, so it will be extruded with a proper pattern.
	        	m_layers[num_layers - 1]->m_regions[region_id]->slices.set_type(stTop);
	        for (size_t i = num_layers; i < m_layers.size(); ++ i)
	        	m_layers[i]->m_regions[region_id]->slices.set_type(stInternal);
        }

        BOOST_LOG_TRIVIAL(debug) << "Detecting solid surfaces for region " << region_id << " - clipping in parallel - start";
        // Fill in layerm->fill_surfaces by trimming the layerm->slices by the cummulative layerm->fill_surfaces.
        tbb::parallel_for(
            tbb::blocked_range<size_t>(0, m_layers.size()),
            [this, region_id](const tbb::blocked_range<size_t>& range) {
                for (size_t idx_layer = range.begin(); idx_layer < range.end(); ++ idx_layer) {
                    m_print->throw_if_canceled();
                    LayerRegion *layerm = m_layers[idx_layer]->m_regions[region_id];
                    layerm->slices_to_fill_surfaces_clipped();
#ifdef SLIC3R_DEBUG_SLICE_PROCESSING
                    layerm->export_region_fill_surfaces_to_svg_debug("1_detect_surfaces_type-final");
#endif /* SLIC3R_DEBUG_SLICE_PROCESSING */
                } // for each layer of a region
            });
        m_print->throw_if_canceled();
        BOOST_LOG_TRIVIAL(debug) << "Detecting solid surfaces for region " << region_id << " - clipping in parallel - end";
    } // for each this->print->region_count

    // Mark the object to have the region slices classified (typed, which also means they are split based on whether they are supported, bridging, top layers etc.)
    m_typed_slices = true;
}

void PrintObject::process_external_surfaces()
{
    BOOST_LOG_TRIVIAL(info) << "Processing external surfaces..." << log_memory_info();

    // Cached surfaces covered by some extrusion, defining regions, over which the from the surfaces one layer higher are allowed to expand.
    std::vector<Polygons> surfaces_covered;
    // Is there any printing region, that has zero infill? If so, then we don't want the expansion to be performed over the complete voids, but only
    // over voids, which are supported by the layer below.
    bool 				  has_voids = false;
	for (size_t region_id = 0; region_id < this->num_printing_regions(); ++ region_id)
		if (this->printing_region(region_id).config().sparse_infill_density == 0) {
			has_voids = true;
			break;
		}
	if (has_voids && m_layers.size() > 1) {
	    // All but stInternal fill surfaces will get expanded and possibly trimmed.
	    std::vector<unsigned char> layer_expansions_and_voids(m_layers.size(), false);
	    for (size_t layer_idx = 0; layer_idx < m_layers.size(); ++ layer_idx) {
	    	const Layer *layer = m_layers[layer_idx];
	    	bool expansions = false;
	    	bool voids      = false;
	    	for (const LayerRegion *layerm : layer->regions()) {
	    		for (const Surface &surface : layerm->fill_surfaces.surfaces) {
	    			if (surface.surface_type == stInternal)
	    				voids = true;
	    			else
	    				expansions = true;
	    			if (voids && expansions) {
	    				layer_expansions_and_voids[layer_idx] = true;
	    				goto end;
	    			}
	    		}
	    	}
		end:;
		}
	    BOOST_LOG_TRIVIAL(debug) << "Collecting surfaces covered with extrusions in parallel - start";
	    surfaces_covered.resize(m_layers.size() - 1, Polygons());
    	auto unsupported_width = - float(scale_(0.3 * EXTERNAL_INFILL_MARGIN));
	    tbb::parallel_for(
	        tbb::blocked_range<size_t>(0, m_layers.size() - 1),
	        [this, &surfaces_covered, &layer_expansions_and_voids, unsupported_width](const tbb::blocked_range<size_t>& range) {
	            for (size_t layer_idx = range.begin(); layer_idx < range.end(); ++ layer_idx)
	            	if (layer_expansions_and_voids[layer_idx + 1]) {
		                m_print->throw_if_canceled();
		                Polygons voids;
		                for (const LayerRegion *layerm : m_layers[layer_idx]->regions()) {
		                	if (layerm->region().config().sparse_infill_density.value == 0.)
		                		for (const Surface &surface : layerm->fill_surfaces.surfaces)
		                			// Shrink the holes, let the layer above expand slightly inside the unsupported areas.
		                			polygons_append(voids, offset(surface.expolygon, unsupported_width));
		                }
		                surfaces_covered[layer_idx] = diff(m_layers[layer_idx]->lslices, voids);
	            	}
	        }
	    );
	    m_print->throw_if_canceled();
	    BOOST_LOG_TRIVIAL(debug) << "Collecting surfaces covered with extrusions in parallel - end";
	}

	for (size_t region_id = 0; region_id < this->num_printing_regions(); ++region_id) {
        BOOST_LOG_TRIVIAL(debug) << "Processing external surfaces for region " << region_id << " in parallel - start";
        tbb::parallel_for(
            tbb::blocked_range<size_t>(0, m_layers.size()),
            [this, &surfaces_covered, region_id](const tbb::blocked_range<size_t>& range) {
                for (size_t layer_idx = range.begin(); layer_idx < range.end(); ++ layer_idx) {
                    m_print->throw_if_canceled();
                    // BOOST_LOG_TRIVIAL(trace) << "Processing external surface, layer" << m_layers[layer_idx]->print_z;
                    m_layers[layer_idx]->get_region(int(region_id))->process_external_surfaces(
                    	(layer_idx == 0) ? nullptr : m_layers[layer_idx - 1],
                    	(layer_idx == 0 || surfaces_covered.empty() || surfaces_covered[layer_idx - 1].empty()) ? nullptr : &surfaces_covered[layer_idx - 1]);
                }
            }
        );
        m_print->throw_if_canceled();
        BOOST_LOG_TRIVIAL(debug) << "Processing external surfaces for region " << region_id << " in parallel - end";
    }
}

void PrintObject::discover_vertical_shells()
{
    PROFILE_FUNC();

    BOOST_LOG_TRIVIAL(info) << "Discovering vertical shells..." << log_memory_info();

    struct DiscoverVerticalShellsCacheEntry
    {
        // Collected polygons, offsetted
        Polygons    top_surfaces;
        Polygons    bottom_surfaces;
        Polygons    holes;
    };
    bool     spiral_mode      = this->print()->config().spiral_mode.value;
    size_t   num_layers       = spiral_mode ? std::min(size_t(this->printing_region(0).config().bottom_shell_layers), m_layers.size()) : m_layers.size();
    coordf_t min_layer_height = this->slicing_parameters().min_layer_height;
    // Does this region possibly produce more than 1 top or bottom layer?
    auto has_extra_layers_fn = [min_layer_height](const PrintRegionConfig &config) {
	    auto num_extra_layers = [min_layer_height](int num_solid_layers, coordf_t min_shell_thickness) {
	    	if (num_solid_layers == 0)
	    		return 0;
	    	int n = num_solid_layers - 1;
	    	int n2 = int(ceil(min_shell_thickness / min_layer_height));
	    	return std::max(n, n2 - 1);
	    };
    	return num_extra_layers(config.top_shell_layers, config.top_shell_thickness) +
	    	   num_extra_layers(config.bottom_shell_layers, config.bottom_shell_thickness) > 0;
    };
    std::vector<DiscoverVerticalShellsCacheEntry> cache_top_botom_regions(num_layers, DiscoverVerticalShellsCacheEntry());
    bool top_bottom_surfaces_all_regions = this->num_printing_regions() > 1 && ! m_config.interface_shells.value;
    if (top_bottom_surfaces_all_regions) {
        // This is a multi-material print and interface_shells are disabled, meaning that the vertical shell thickness
        // is calculated over all materials.
        // Is the "ensure vertical wall thickness" applicable to any region?
        bool has_extra_layers = false;
        for (size_t region_id = 0; region_id < this->num_printing_regions(); ++region_id) {
            const PrintRegionConfig &config = this->printing_region(region_id).config();
            if (config.ensure_vertical_shell_thickness.value && has_extra_layers_fn(config)) {
                has_extra_layers = true;
                break;
            }
        }
        if (! has_extra_layers)
            // The "ensure vertical wall thickness" feature is not applicable to any of the regions. Quit.
            return;
        BOOST_LOG_TRIVIAL(debug) << "Discovering vertical shells in parallel - start : cache top / bottom";
        //FIXME Improve the heuristics for a grain size.
        size_t grain_size = std::max(num_layers / 16, size_t(1));
        tbb::parallel_for(
            tbb::blocked_range<size_t>(0, num_layers, grain_size),
            [this, &cache_top_botom_regions](const tbb::blocked_range<size_t>& range) {
                const SurfaceType surfaces_bottom[2] = { stBottom, stBottomBridge };
                const size_t num_regions = this->num_printing_regions();
                for (size_t idx_layer = range.begin(); idx_layer < range.end(); ++ idx_layer) {
                    m_print->throw_if_canceled();
                    const Layer                      &layer = *m_layers[idx_layer];
                    DiscoverVerticalShellsCacheEntry &cache = cache_top_botom_regions[idx_layer];
                    // Simulate single set of perimeters over all merged regions.
                    float                             perimeter_offset = 0.f;
                    float                             perimeter_min_spacing = FLT_MAX;
#ifdef SLIC3R_DEBUG_SLICE_PROCESSING
                    static size_t debug_idx = 0;
                    ++ debug_idx;
#endif /* SLIC3R_DEBUG_SLICE_PROCESSING */
                    for (size_t region_id = 0; region_id < num_regions; ++ region_id) {
                        LayerRegion &layerm                       = *layer.m_regions[region_id];
                        float        min_perimeter_infill_spacing = float(layerm.flow(frSolidInfill).scaled_spacing()) * 1.05f;
                        // Top surfaces.
                        append(cache.top_surfaces, offset(layerm.slices.filter_by_type(stTop), min_perimeter_infill_spacing));
                        append(cache.top_surfaces, offset(layerm.fill_surfaces.filter_by_type(stTop), min_perimeter_infill_spacing));
                        // Bottom surfaces.
                        append(cache.bottom_surfaces, offset(layerm.slices.filter_by_types(surfaces_bottom, 2), min_perimeter_infill_spacing));
                        append(cache.bottom_surfaces, offset(layerm.fill_surfaces.filter_by_types(surfaces_bottom, 2), min_perimeter_infill_spacing));
                        // Calculate the maximum perimeter offset as if the slice was extruded with a single extruder only.
                        // First find the maxium number of perimeters per region slice.
                        unsigned int perimeters = 0;
                        for (Surface &s : layerm.slices.surfaces)
                            perimeters = std::max<unsigned int>(perimeters, s.extra_perimeters);
                        perimeters += layerm.region().config().wall_loops.value;
                        // Then calculate the infill offset.
                        if (perimeters > 0) {
                            Flow extflow = layerm.flow(frExternalPerimeter);
                            Flow flow    = layerm.flow(frPerimeter);
                            perimeter_offset = std::max(perimeter_offset,
                                0.5f * float(extflow.scaled_width() + extflow.scaled_spacing()) + (float(perimeters) - 1.f) * flow.scaled_spacing());
                            perimeter_min_spacing = std::min(perimeter_min_spacing, float(std::min(extflow.scaled_spacing(), flow.scaled_spacing())));
                        }
                        polygons_append(cache.holes, to_polygons(layerm.fill_expolygons));
                    }
                    // Save some computing time by reducing the number of polygons.
                    cache.top_surfaces    = union_(cache.top_surfaces);
                    cache.bottom_surfaces = union_(cache.bottom_surfaces);
                    // For a multi-material print, simulate perimeter / infill split as if only a single extruder has been used for the whole print.
                    if (perimeter_offset > 0.) {
                        // The layer.lslices are forced to merge by expanding them first.
                        polygons_append(cache.holes, offset2(layer.lslices, 0.3f * perimeter_min_spacing, - perimeter_offset - 0.3f * perimeter_min_spacing));
#ifdef SLIC3R_DEBUG_SLICE_PROCESSING
                        {
                            Slic3r::SVG svg(debug_out_path("discover_vertical_shells-extra-holes-%d.svg", debug_idx), get_extents(layer.lslices));
                            svg.draw(layer.lslices, "blue");
                            svg.draw(union_ex(cache.holes), "red");
                            svg.draw_outline(union_ex(cache.holes), "black", "blue", scale_(0.05));
                            svg.Close();
                        }
#endif /* SLIC3R_DEBUG_SLICE_PROCESSING */
                    }
                    cache.holes = union_(cache.holes);
                }
            });
        m_print->throw_if_canceled();
        BOOST_LOG_TRIVIAL(debug) << "Discovering vertical shells in parallel - end : cache top / bottom";
    }

    for (size_t region_id = 0; region_id < this->num_printing_regions(); ++ region_id) {
        PROFILE_BLOCK(discover_vertical_shells_region);

        const PrintRegion &region = this->printing_region(region_id);
        if (! region.config().ensure_vertical_shell_thickness.value)
            // This region will be handled by discover_horizontal_shells().
            continue;
        if (! has_extra_layers_fn(region.config()))
            // Zero or 1 layer, there is no additional vertical wall thickness enforced.
            continue;

        //FIXME Improve the heuristics for a grain size.
        size_t grain_size = std::max(num_layers / 16, size_t(1));

        if (! top_bottom_surfaces_all_regions) {
            // This is either a single material print, or a multi-material print and interface_shells are enabled, meaning that the vertical shell thickness
            // is calculated over a single material.
            BOOST_LOG_TRIVIAL(debug) << "Discovering vertical shells for region " << region_id << " in parallel - start : cache top / bottom";
            tbb::parallel_for(
                tbb::blocked_range<size_t>(0, num_layers, grain_size),
                [this, region_id, &cache_top_botom_regions](const tbb::blocked_range<size_t>& range) {
                    const SurfaceType surfaces_bottom[2] = { stBottom, stBottomBridge };
                    for (size_t idx_layer = range.begin(); idx_layer < range.end(); ++ idx_layer) {
                        m_print->throw_if_canceled();
                        Layer       &layer                        = *m_layers[idx_layer];
                        LayerRegion &layerm                       = *layer.m_regions[region_id];
                        float        min_perimeter_infill_spacing = float(layerm.flow(frSolidInfill).scaled_spacing()) * 1.05f;
                        // Top surfaces.
                        auto &cache = cache_top_botom_regions[idx_layer];
                        cache.top_surfaces = offset(layerm.slices.filter_by_type(stTop), min_perimeter_infill_spacing);
                        append(cache.top_surfaces, offset(layerm.fill_surfaces.filter_by_type(stTop), min_perimeter_infill_spacing));
                        // Bottom surfaces.
                        cache.bottom_surfaces = offset(layerm.slices.filter_by_types(surfaces_bottom, 2), min_perimeter_infill_spacing);
                        append(cache.bottom_surfaces, offset(layerm.fill_surfaces.filter_by_types(surfaces_bottom, 2), min_perimeter_infill_spacing));
                        // Holes over all regions. Only collect them once, they are valid for all region_id iterations.
                        if (cache.holes.empty()) {
                            for (size_t region_id = 0; region_id < layer.regions().size(); ++ region_id)
                                polygons_append(cache.holes, to_polygons(layer.regions()[region_id]->fill_expolygons));
                        }
                    }
                });
            m_print->throw_if_canceled();
            BOOST_LOG_TRIVIAL(debug) << "Discovering vertical shells for region " << region_id << " in parallel - end : cache top / bottom";
        }

        BOOST_LOG_TRIVIAL(debug) << "Discovering vertical shells for region " << region_id << " in parallel - start : ensure vertical wall thickness";
        tbb::parallel_for(
            tbb::blocked_range<size_t>(0, num_layers, grain_size),
            [this, region_id, &cache_top_botom_regions]
            (const tbb::blocked_range<size_t>& range) {
                // printf("discover_vertical_shells from %d to %d\n", range.begin(), range.end());
                for (size_t idx_layer = range.begin(); idx_layer < range.end(); ++ idx_layer) {
                    PROFILE_BLOCK(discover_vertical_shells_region_layer);
                    m_print->throw_if_canceled();
#ifdef SLIC3R_DEBUG_SLICE_PROCESSING
        			static size_t debug_idx = 0;
        			++ debug_idx;
#endif /* SLIC3R_DEBUG_SLICE_PROCESSING */

                    Layer       	        *layer          = m_layers[idx_layer];
                    LayerRegion 	        *layerm         = layer->m_regions[region_id];
                    const PrintRegionConfig &region_config  = layerm->region().config();

#ifdef SLIC3R_DEBUG_SLICE_PROCESSING
                    layerm->export_region_slices_to_svg_debug("4_discover_vertical_shells-initial");
                    layerm->export_region_fill_surfaces_to_svg_debug("4_discover_vertical_shells-initial");
#endif /* SLIC3R_DEBUG_SLICE_PROCESSING */

                    Flow         solid_infill_flow   = layerm->flow(frSolidInfill);
                    coord_t      infill_line_spacing = solid_infill_flow.scaled_spacing();
                    // Find a union of perimeters below / above this surface to guarantee a minimum shell thickness.
                    Polygons shell;
                    Polygons holes;
#ifdef SLIC3R_DEBUG_SLICE_PROCESSING
                    ExPolygons shell_ex;
#endif /* SLIC3R_DEBUG_SLICE_PROCESSING */
                    float min_perimeter_infill_spacing = float(infill_line_spacing) * 1.05f;
                    {
                        PROFILE_BLOCK(discover_vertical_shells_region_layer_collect);
#if 0
// #ifdef SLIC3R_DEBUG_SLICE_PROCESSING
                        {
        					Slic3r::SVG svg_cummulative(debug_out_path("discover_vertical_shells-perimeters-before-union-run%d.svg", debug_idx), this->bounding_box());
                            for (int n = (int)idx_layer - n_extra_bottom_layers; n <= (int)idx_layer + n_extra_top_layers; ++ n) {
                                if (n < 0 || n >= (int)m_layers.size())
                                    continue;
                                ExPolygons &expolys = m_layers[n]->perimeter_expolygons;
                                for (size_t i = 0; i < expolys.size(); ++ i) {
        							Slic3r::SVG svg(debug_out_path("discover_vertical_shells-perimeters-before-union-run%d-layer%d-expoly%d.svg", debug_idx, n, i), get_extents(expolys[i]));
                                    svg.draw(expolys[i]);
                                    svg.draw_outline(expolys[i].contour, "black", scale_(0.05));
                                    svg.draw_outline(expolys[i].holes, "blue", scale_(0.05));
                                    svg.Close();

                                    svg_cummulative.draw(expolys[i]);
                                    svg_cummulative.draw_outline(expolys[i].contour, "black", scale_(0.05));
                                    svg_cummulative.draw_outline(expolys[i].holes, "blue", scale_(0.05));
                                }
                            }
                        }
#endif /* SLIC3R_DEBUG_SLICE_PROCESSING */
			        	polygons_append(holes, cache_top_botom_regions[idx_layer].holes);
			        	if (int n_top_layers = region_config.top_shell_layers.value; n_top_layers > 0) {
                            // Gather top regions projected to this layer.
                            coordf_t print_z = layer->print_z;
	                        for (int i = int(idx_layer) + 1;
	                        	i < int(cache_top_botom_regions.size()) &&
	                        		(i < int(idx_layer) + n_top_layers ||
	                        		 m_layers[i]->print_z - print_z < region_config.top_shell_thickness - EPSILON);
	                        	++ i) {
	                            const DiscoverVerticalShellsCacheEntry &cache = cache_top_botom_regions[i];
								if (! holes.empty())
									holes = intersection(holes, cache.holes);
								if (! cache.top_surfaces.empty()) {
		                            polygons_append(shell, cache.top_surfaces);
		                            // Running the union_ using the Clipper library piece by piece is cheaper
		                            // than running the union_ all at once.
	                               shell = union_(shell);
	                           }
	                        }
	                    }
	                    if (int n_bottom_layers = region_config.bottom_shell_layers.value; n_bottom_layers > 0) {
                            // Gather bottom regions projected to this layer.
                            coordf_t bottom_z = layer->bottom_z();
	                        for (int i = int(idx_layer) - 1;
	                        	i >= 0 &&
	                        		(i > int(idx_layer) - n_bottom_layers ||
	                        		 bottom_z - m_layers[i]->bottom_z() < region_config.bottom_shell_thickness - EPSILON);
	                        	-- i) {
	                            const DiscoverVerticalShellsCacheEntry &cache = cache_top_botom_regions[i];
								if (! holes.empty())
									holes = intersection(holes, cache.holes);
								if (! cache.bottom_surfaces.empty()) {
		                            polygons_append(shell, cache.bottom_surfaces);
		                            // Running the union_ using the Clipper library piece by piece is cheaper
		                            // than running the union_ all at once.
		                            shell = union_(shell);
		                        }
	                        }
	                    }
#ifdef SLIC3R_DEBUG_SLICE_PROCESSING
                        {
        					Slic3r::SVG svg(debug_out_path("discover_vertical_shells-perimeters-before-union-%d.svg", debug_idx), get_extents(shell));
                            svg.draw(shell);
                            svg.draw_outline(shell, "black", scale_(0.05));
                            svg.Close();
                        }
#endif /* SLIC3R_DEBUG_SLICE_PROCESSING */
#if 0
                        {
                            PROFILE_BLOCK(discover_vertical_shells_region_layer_shell_);
        //                    shell = union_(shell, true);
                            shell = union_(shell, false);
                        }
#endif
#ifdef SLIC3R_DEBUG_SLICE_PROCESSING
                        shell_ex = union_safety_offset_ex(shell);
#endif /* SLIC3R_DEBUG_SLICE_PROCESSING */
                    }

                    //if (shell.empty())
                    //    continue;

#ifdef SLIC3R_DEBUG_SLICE_PROCESSING
                    {
                        Slic3r::SVG svg(debug_out_path("discover_vertical_shells-perimeters-after-union-%d.svg", debug_idx), get_extents(shell));
                        svg.draw(shell_ex);
                        svg.draw_outline(shell_ex, "black", "blue", scale_(0.05));
                        svg.Close();
                    }
#endif /* SLIC3R_DEBUG_SLICE_PROCESSING */

#ifdef SLIC3R_DEBUG_SLICE_PROCESSING
                    {
                        Slic3r::SVG svg(debug_out_path("discover_vertical_shells-internal-wshell-%d.svg", debug_idx), get_extents(shell));
                        svg.draw(layerm->fill_surfaces.filter_by_type(stInternal), "yellow", 0.5);
                        svg.draw_outline(layerm->fill_surfaces.filter_by_type(stInternal), "black", "blue", scale_(0.05));
                        svg.draw(shell_ex, "blue", 0.5);
                        svg.draw_outline(shell_ex, "black", "blue", scale_(0.05));
                        svg.Close();
                    }
                    {
                        Slic3r::SVG svg(debug_out_path("discover_vertical_shells-internalvoid-wshell-%d.svg", debug_idx), get_extents(shell));
                        svg.draw(layerm->fill_surfaces.filter_by_type(stInternalVoid), "yellow", 0.5);
                        svg.draw_outline(layerm->fill_surfaces.filter_by_type(stInternalVoid), "black", "blue", scale_(0.05));
                        svg.draw(shell_ex, "blue", 0.5);
                        svg.draw_outline(shell_ex, "black", "blue", scale_(0.05));
                        svg.Close();
                    }
                    {
                        Slic3r::SVG svg(debug_out_path("discover_vertical_shells-internalvoid-wshell-%d.svg", debug_idx), get_extents(shell));
                        svg.draw(layerm->fill_surfaces.filter_by_type(stInternalVoid), "yellow", 0.5);
                        svg.draw_outline(layerm->fill_surfaces.filter_by_type(stInternalVoid), "black", "blue", scale_(0.05));
                        svg.draw(shell_ex, "blue", 0.5);
                        svg.draw_outline(shell_ex, "black", "blue", scale_(0.05));
                        svg.Close();
                    }
#endif /* SLIC3R_DEBUG_SLICE_PROCESSING */

                    // Trim the shells region by the internal & internal void surfaces.
                    const SurfaceType surfaceTypesInternal[] = { stInternal, stInternalVoid, stInternalSolid };
                    const Polygons    polygonsInternal = to_polygons(layerm->fill_surfaces.filter_by_types(surfaceTypesInternal, 3));
                    shell = intersection(shell, polygonsInternal, ApplySafetyOffset::Yes);
                    polygons_append(shell, diff(polygonsInternal, holes));
                    if (shell.empty())
                        continue;

                    // Append the internal solids, so they will be merged with the new ones.
                    polygons_append(shell, to_polygons(layerm->fill_surfaces.filter_by_type(stInternalSolid)));

                    // These regions will be filled by a rectilinear full infill. Currently this type of infill
                    // only fills regions, which fit at least a single line. To avoid gaps in the sparse infill,
                    // make sure that this region does not contain parts narrower than the infill spacing width.
#ifdef SLIC3R_DEBUG_SLICE_PROCESSING
                    Polygons shell_before = shell;
#endif /* SLIC3R_DEBUG_SLICE_PROCESSING */
#if 1
                    // Intentionally inflate a bit more than how much the region has been shrunk,
                    // so there will be some overlap between this solid infill and the other infill regions (mainly the sparse infill).
                    shell = opening(union_(shell), 0.5f * min_perimeter_infill_spacing, 0.8f * min_perimeter_infill_spacing, ClipperLib::jtSquare);
                    if (shell.empty())
                        continue;
#else
                    // Ensure each region is at least 3x infill line width wide, so it could be filled in.
        //            float margin = float(infill_line_spacing) * 3.f;
                    float margin = float(infill_line_spacing) * 1.5f;
                    // we use a higher miterLimit here to handle areas with acute angles
                    // in those cases, the default miterLimit would cut the corner and we'd
                    // get a triangle in $too_narrow; if we grow it below then the shell
                    // would have a different shape from the external surface and we'd still
                    // have the same angle, so the next shell would be grown even more and so on.
                    Polygons too_narrow = diff(shell, opening(shell, margin, ClipperLib::jtMiter, 5.), true);
                    if (! too_narrow.empty()) {
                        // grow the collapsing parts and add the extra area to  the neighbor layer
                        // as well as to our original surfaces so that we support this
                        // additional area in the next shell too
                        // make sure our grown surfaces don't exceed the fill area
                        polygons_append(shell, intersection(offset(too_narrow, margin), polygonsInternal));
                    }
#endif
                    ExPolygons new_internal_solid = intersection_ex(polygonsInternal, shell);
#ifdef SLIC3R_DEBUG_SLICE_PROCESSING
                    {
                        Slic3r::SVG svg(debug_out_path("discover_vertical_shells-regularized-%d.svg", debug_idx), get_extents(shell_before));
                        // Source shell.
                        svg.draw(union_safety_offset_ex(shell_before));
                        // Shell trimmed to the internal surfaces.
                        svg.draw_outline(union_safety_offset_ex(shell), "black", "blue", scale_(0.05));
                        // Regularized infill region.
                        svg.draw_outline(new_internal_solid, "red", "magenta", scale_(0.05));
                        svg.Close();
                    }
#endif /* SLIC3R_DEBUG_SLICE_PROCESSING */

                    // Trim the internal & internalvoid by the shell.
                    Slic3r::ExPolygons new_internal = diff_ex(layerm->fill_surfaces.filter_by_type(stInternal), shell);
                    Slic3r::ExPolygons new_internal_void = diff_ex(layerm->fill_surfaces.filter_by_type(stInternalVoid), shell);

#ifdef SLIC3R_DEBUG_SLICE_PROCESSING
                    {
                        SVG::export_expolygons(debug_out_path("discover_vertical_shells-new_internal-%d.svg", debug_idx), get_extents(shell), new_internal, "black", "blue", scale_(0.05));
        				SVG::export_expolygons(debug_out_path("discover_vertical_shells-new_internal_void-%d.svg", debug_idx), get_extents(shell), new_internal_void, "black", "blue", scale_(0.05));
        				SVG::export_expolygons(debug_out_path("discover_vertical_shells-new_internal_solid-%d.svg", debug_idx), get_extents(shell), new_internal_solid, "black", "blue", scale_(0.05));
                    }
#endif /* SLIC3R_DEBUG_SLICE_PROCESSING */

                    // Assign resulting internal surfaces to layer.
                    const SurfaceType surfaceTypesKeep[] = { stTop, stBottom, stBottomBridge };
                    layerm->fill_surfaces.keep_types(surfaceTypesKeep, sizeof(surfaceTypesKeep)/sizeof(SurfaceType));
                    layerm->fill_surfaces.append(new_internal,       stInternal);
                    layerm->fill_surfaces.append(new_internal_void,  stInternalVoid);
                    layerm->fill_surfaces.append(new_internal_solid, stInternalSolid);
                } // for each layer
            });
        m_print->throw_if_canceled();
        BOOST_LOG_TRIVIAL(debug) << "Discovering vertical shells for region " << region_id << " in parallel - end";

#ifdef SLIC3R_DEBUG_SLICE_PROCESSING
		for (size_t idx_layer = 0; idx_layer < m_layers.size(); ++idx_layer) {
			LayerRegion *layerm = m_layers[idx_layer]->get_region(region_id);
			layerm->export_region_slices_to_svg_debug("4_discover_vertical_shells-final");
			layerm->export_region_fill_surfaces_to_svg_debug("4_discover_vertical_shells-final");
		}
#endif /* SLIC3R_DEBUG_SLICE_PROCESSING */
    } // for each region

    // Write the profiler measurements to file
//    PROFILE_UPDATE();
//    PROFILE_OUTPUT(debug_out_path("discover_vertical_shells-profile.txt").c_str());
}

/* This method applies bridge flow to the first internal solid layer above
   sparse infill */
void PrintObject::bridge_over_infill()
{
    BOOST_LOG_TRIVIAL(info) << "Bridge over infill..." << log_memory_info();

    for (size_t region_id = 0; region_id < this->num_printing_regions(); ++ region_id) {
        const PrintRegion &region = this->printing_region(region_id);

        // skip bridging in case there are no voids
        if (region.config().sparse_infill_density.value == 100)
            continue;

		for (LayerPtrs::iterator layer_it = m_layers.begin(); layer_it != m_layers.end(); ++ layer_it) {
            // skip first layer
			if (layer_it == m_layers.begin())
                continue;

            Layer       *layer       = *layer_it;
            LayerRegion *layerm      = layer->m_regions[region_id];
            const PrintObjectConfig& object_config = layer->object()->config();
            //BBS: enable thick bridge for internal bridge only
            Flow         bridge_flow = layerm->bridging_flow(frSolidInfill, true);

            // extract the stInternalSolid surfaces that might be transformed into bridges
            Polygons internal_solid;
            layerm->fill_surfaces.filter_by_type(stInternalSolid, &internal_solid);

            // check whether the lower area is deep enough for absorbing the extra flow
            // (for obvious physical reasons but also for preventing the bridge extrudates
            // from overflowing in 3D preview)
            ExPolygons to_bridge;
            {
                Polygons to_bridge_pp = internal_solid;

                // iterate through lower layers spanned by bridge_flow
                double bottom_z = layer->print_z - bridge_flow.height() - EPSILON;
                for (int i = int(layer_it - m_layers.begin()) - 1; i >= 0; --i) {
                    const Layer* lower_layer = m_layers[i];

                    // stop iterating if layer is lower than bottom_z
                    if (lower_layer->print_z < bottom_z) break;

                    // iterate through regions and collect internal surfaces
                    Polygons lower_internal;
                    for (LayerRegion *lower_layerm : lower_layer->m_regions)
                        lower_layerm->fill_surfaces.filter_by_type(stInternal, &lower_internal);

                    // intersect such lower internal surfaces with the candidate solid surfaces
                    to_bridge_pp = intersection(to_bridge_pp, lower_internal);
                }

                // BBS: expand to make avoid gap between bridge and inner wall
                to_bridge_pp = expand(to_bridge_pp, bridge_flow.scaled_width());
                to_bridge_pp = intersection(to_bridge_pp, internal_solid);

                // there's no point in bridging too thin/short regions
                //FIXME Vojtech: The offset2 function is not a geometric offset,
                // therefore it may create 1) gaps, and 2) sharp corners, which are outside the original contour.
                // The gaps will be filled by a separate region, which makes the infill less stable and it takes longer.
                {
                    float min_width = float(bridge_flow.scaled_width()) * 3.f;
                    to_bridge_pp = opening(to_bridge_pp, min_width);
                }

                if (to_bridge_pp.empty()) continue;

                // convert into ExPolygons
                to_bridge = union_ex(to_bridge_pp);
            }

            #ifdef SLIC3R_DEBUG
            printf("Bridging %zu internal areas at layer %zu\n", to_bridge.size(), layer->id());
            #endif

            // compute the remaning internal solid surfaces as difference
            ExPolygons not_to_bridge = diff_ex(internal_solid, to_bridge, ApplySafetyOffset::Yes);
            to_bridge = intersection_ex(to_bridge, internal_solid, ApplySafetyOffset::Yes);
            // build the new collection of fill_surfaces
            layerm->fill_surfaces.remove_type(stInternalSolid);
            for (ExPolygon &ex : to_bridge) {
                layerm->fill_surfaces.surfaces.push_back(Surface(stInternalBridge, ex));
                // BBS: detect angle for internal bridge infill
                InternalBridgeDetector ibd(ex, layerm->fill_no_overlap_expolygons, bridge_flow.scaled_spacing());
                if (ibd.detect_angle()) {
                    (layerm->fill_surfaces.surfaces.end() - 1)->bridge_angle = ibd.angle;
                }
            }

            for (ExPolygon &ex : not_to_bridge)
                layerm->fill_surfaces.surfaces.push_back(Surface(stInternalSolid, ex));

            //BBS: modify stInternal to be stInternalWithLoop to give better support to internal bridge
            if (!to_bridge.empty()){
                float internal_loop_thickness = object_config.internal_bridge_support_thickness.value;
                double bottom_z = layer->print_z - layer->height - internal_loop_thickness + EPSILON;
                //BBS: lighting infill doesn't support this feature. Don't need to add loop when infill density is high than 50%
                if (region.config().sparse_infill_pattern != InfillPattern::ipLightning && region.config().sparse_infill_density.value < 50)
                    for (int i = int(layer_it - m_layers.begin()) - 1; i >= 0; --i) {
                        const Layer* lower_layer = m_layers[i];

                        if (lower_layer->print_z < bottom_z) break;

                        for (LayerRegion* lower_layerm : lower_layer->m_regions) {
                            Polygons lower_internal;
                            lower_layerm->fill_surfaces.filter_by_type(stInternal, &lower_internal);
                            ExPolygons internal_with_loop = intersection_ex(lower_internal, to_bridge);
                            ExPolygons internal = diff_ex(lower_internal, to_bridge);
                            if (internal_with_loop.empty()) {
                                //BBS: don't need to do anything
                            }
                            else if (internal.empty()) {
                                lower_layerm->fill_surfaces.change_to_new_type(stInternal, stInternalWithLoop);
                            }
                            else {
                                lower_layerm->fill_surfaces.remove_type(stInternal);
                                for (ExPolygon& ex : internal_with_loop)
                                    lower_layerm->fill_surfaces.surfaces.push_back(Surface(stInternalWithLoop, ex));
                                for (ExPolygon& ex : internal)
                                    lower_layerm->fill_surfaces.surfaces.push_back(Surface(stInternal, ex));
                            }
                        }
                    }
            }

            /*
            # exclude infill from the layers below if needed
            # see discussion at https://github.com/alexrj/Slic3r/issues/240
            # Update: do not exclude any infill. Sparse infill is able to absorb the excess material.
            if (0) {
                my $excess = $layerm->extruders->{infill}->bridge_flow->width - $layerm->height;
                for (my $i = $layer_id-1; $excess >= $self->get_layer($i)->height; $i--) {
                    Slic3r::debugf "  skipping infill below those areas at layer %d\n", $i;
                    foreach my $lower_layerm (@{$self->get_layer($i)->regions}) {
                        my @new_surfaces = ();
                        # subtract the area from all types of surfaces
                        foreach my $group (@{$lower_layerm->fill_surfaces->group}) {
                            push @new_surfaces, map $group->[0]->clone(expolygon => $_),
                                @{diff_ex(
                                    [ map $_->p, @$group ],
                                    [ map @$_, @$to_bridge ],
                                )};
                            push @new_surfaces, map Slic3r::Surface->new(
                                expolygon       => $_,
                                surface_type    => stInternalVoid,
                            ), @{intersection_ex(
                                [ map $_->p, @$group ],
                                [ map @$_, @$to_bridge ],
                            )};
                        }
                        $lower_layerm->fill_surfaces->clear;
                        $lower_layerm->fill_surfaces->append($_) for @new_surfaces;
                    }

                    $excess -= $self->get_layer($i)->height;
                }
            }
            */

#ifdef SLIC3R_DEBUG_SLICE_PROCESSING
            layerm->export_region_slices_to_svg_debug("7_bridge_over_infill");
            layerm->export_region_fill_surfaces_to_svg_debug("7_bridge_over_infill");
#endif /* SLIC3R_DEBUG_SLICE_PROCESSING */
            m_print->throw_if_canceled();
        }
    }
}

static void clamp_exturder_to_default(ConfigOptionInt &opt, size_t num_extruders)
{
    if (opt.value > (int)num_extruders)
        // assign the default extruder
        opt.value = 1;
}

PrintObjectConfig PrintObject::object_config_from_model_object(const PrintObjectConfig &default_object_config, const ModelObject &object, size_t num_extruders)
{
    PrintObjectConfig config = default_object_config;
    {
        DynamicPrintConfig src_normalized(object.config.get());
        src_normalized.normalize_fdm();
        config.apply(src_normalized, true);
    }
    // Clamp invalid extruders to the default extruder (with index 1).
    clamp_exturder_to_default(config.support_filament,           num_extruders);
    clamp_exturder_to_default(config.support_interface_filament, num_extruders);
    return config;
}

const std::string                                                    key_extruder { "extruder" };
static constexpr const std::initializer_list<const std::string_view> keys_extruders { "sparse_infill_filament"sv, "solid_infill_filament"sv, "wall_filament"sv };

static void apply_to_print_region_config(PrintRegionConfig &out, const DynamicPrintConfig &in)
{
    // 1) Copy the "extruder key to sparse_infill_filament and wall_filament.
    auto *opt_extruder = in.opt<ConfigOptionInt>(key_extruder);
    if (opt_extruder)
        if (int extruder = opt_extruder->value; extruder != 0) {
            // Not a default extruder.
            out.sparse_infill_filament      .value = extruder;
            out.solid_infill_filament.value = extruder;
            out.wall_filament   .value = extruder;
        }
    // 2) Copy the rest of the values.
    for (auto it = in.cbegin(); it != in.cend(); ++ it)
        if (it->first != key_extruder)
            if (ConfigOption* my_opt = out.option(it->first, false); my_opt != nullptr) {
                if (one_of(it->first, keys_extruders)) {
                    // Ignore "default" extruders.
                    int extruder = static_cast<const ConfigOptionInt*>(it->second.get())->value;
                    if (extruder > 0)
                        my_opt->setInt(extruder);
                } else
                    my_opt->set(it->second.get());
            }
}

PrintRegionConfig region_config_from_model_volume(const PrintRegionConfig &default_or_parent_region_config, const DynamicPrintConfig *layer_range_config, const ModelVolume &volume, size_t num_extruders)
{
    PrintRegionConfig config = default_or_parent_region_config;
    if (volume.is_model_part()) {
        // default_or_parent_region_config contains the Print's PrintRegionConfig.
        // Override with ModelObject's PrintRegionConfig values.
        apply_to_print_region_config(config, volume.get_object()->config.get());
    } else {
        // default_or_parent_region_config contains parent PrintRegion config, which already contains ModelVolume's config.
    }
    if (layer_range_config != nullptr) {
        // Not applicable to modifiers.
        assert(volume.is_model_part());
    	apply_to_print_region_config(config, *layer_range_config);
    }
    apply_to_print_region_config(config, volume.config.get());
    if (! volume.material_id().empty())
        apply_to_print_region_config(config, volume.material()->config.get());
    // Clamp invalid extruders to the default extruder (with index 1).
    clamp_exturder_to_default(config.sparse_infill_filament,       num_extruders);
    clamp_exturder_to_default(config.wall_filament,    num_extruders);
    clamp_exturder_to_default(config.solid_infill_filament, num_extruders);
    if (config.sparse_infill_density.value < 0.00011f)
        // Switch of infill for very low infill rates, also avoid division by zero in infill generator for these very low rates.
        // See GH issue #5910.
        config.sparse_infill_density.value = 0;
    else
        config.sparse_infill_density.value = std::min(config.sparse_infill_density.value, 100.);
    if (config.fuzzy_skin.value != FuzzySkinType::None && (config.fuzzy_skin_point_distance.value < 0.01 || config.fuzzy_skin_thickness.value < 0.001))
        config.fuzzy_skin.value = FuzzySkinType::None;
    return config;
}

struct POProfiler
{
    uint32_t duration1;
    uint32_t duration2;
};

void PrintObject::generate_support_preview()
{
    POProfiler profiler;

    boost::posix_time::ptime ts1 = boost::posix_time::microsec_clock::local_time();
    this->slice();
    boost::posix_time::ptime ts2 = boost::posix_time::microsec_clock::local_time();
    profiler.duration1 = (ts2 - ts1).total_milliseconds();

    this->generate_support_material();
    boost::posix_time::ptime ts3 = boost::posix_time::microsec_clock::local_time();
    profiler.duration2 = (ts3 - ts2).total_milliseconds();
}

void PrintObject::update_slicing_parameters()
{
    if (!m_slicing_params.valid)
        m_slicing_params = SlicingParameters::create_from_config(
            this->print()->config(), m_config, this->model_object()->bounding_box().max.z(), this->object_extruders());
}

SlicingParameters PrintObject::slicing_parameters(const DynamicPrintConfig& full_config, const ModelObject& model_object, float object_max_z)
{
	PrintConfig         print_config;
	PrintObjectConfig   object_config;
	PrintRegionConfig   default_region_config;
	print_config.apply(full_config, true);
	object_config.apply(full_config, true);
	default_region_config.apply(full_config, true);
    // BBS
	size_t              filament_extruders = print_config.filament_diameter.size();
	object_config = object_config_from_model_object(object_config, model_object, filament_extruders);

	std::vector<unsigned int> object_extruders;
	for (const ModelVolume* model_volume : model_object.volumes)
		if (model_volume->is_model_part()) {
			PrintRegion::collect_object_printing_extruders(
				print_config,
				region_config_from_model_volume(default_region_config, nullptr, *model_volume, filament_extruders),
                object_config.brim_type != btNoBrim && object_config.brim_width > 0.,
				object_extruders);
			for (const std::pair<const t_layer_height_range, ModelConfig> &range_and_config : model_object.layer_config_ranges)
				if (range_and_config.second.has("wall_filament") ||
					range_and_config.second.has("sparse_infill_filament") ||
					range_and_config.second.has("solid_infill_filament"))
					PrintRegion::collect_object_printing_extruders(
						print_config,
						region_config_from_model_volume(default_region_config, &range_and_config.second.get(), *model_volume, filament_extruders),
                        object_config.brim_type != btNoBrim && object_config.brim_width > 0.,
						object_extruders);
		}
    sort_remove_duplicates(object_extruders);
    //FIXME add painting extruders

    if (object_max_z <= 0.f)
        object_max_z = (float)model_object.raw_bounding_box().size().z();
    return SlicingParameters::create_from_config(print_config, object_config, object_max_z, object_extruders);
}

// returns 0-based indices of extruders used to print the object (without brim, support and other helper extrusions)
std::vector<unsigned int> PrintObject::object_extruders() const
{
    std::vector<unsigned int> extruders;
    extruders.reserve(this->all_regions().size() * 3);
#if 0
    for (const PrintRegion &region : this->all_regions())
        region.collect_object_printing_extruders(*this->print(), extruders);
#else
    const ModelObject* mo = this->model_object();
    for (const ModelVolume* mv : mo->volumes) {
        std::vector<int> volume_extruders = mv->get_extruders();
        for (int extruder : volume_extruders) {
            assert(extruder > 0);
            extruders.push_back(extruder - 1);
        }
    }
#endif
    sort_remove_duplicates(extruders);
    return extruders;
}

bool PrintObject::update_layer_height_profile(const ModelObject &model_object, const SlicingParameters &slicing_parameters, std::vector<coordf_t> &layer_height_profile)
{
    bool updated = false;

    if (layer_height_profile.empty()) {
        // use the constructor because the assignement is crashing on ASAN OsX
        layer_height_profile = std::vector<coordf_t>(model_object.layer_height_profile.get());
//        layer_height_profile = model_object.layer_height_profile;
        updated = true;
    }

    // Verify the layer_height_profile.
    if (!layer_height_profile.empty() &&
        // Must not be of even length.
        ((layer_height_profile.size() & 1) != 0 ||
            // Last entry must be at the top of the object.
            std::abs(layer_height_profile[layer_height_profile.size() - 2] - slicing_parameters.object_print_z_max + slicing_parameters.object_print_z_min) > 1e-3))
        layer_height_profile.clear();

    if (layer_height_profile.empty() || layer_height_profile[1] != slicing_parameters.first_object_layer_height) {
        //layer_height_profile = layer_height_profile_adaptive(slicing_parameters, model_object.layer_config_ranges, model_object.volumes);
        layer_height_profile = layer_height_profile_from_ranges(slicing_parameters, model_object.layer_config_ranges);
        updated = true;
    }

    return updated;
}
//BBS:
void PrintObject::get_certain_layers(float start, float end, std::vector<LayerPtrs> &out, std::vector<BoundingBox> &boundingbox_objects)
{
    BoundingBox temp;
    LayerPtrs   out_temp;
    for (const auto &layer : layers()) {
        if (layer->print_z < start) continue;

        if (layer->print_z > end + EPSILON) break;
        temp.merge(layer->loverhangs_bbox);
        out_temp.emplace_back(layer);
    }
    boundingbox_objects.emplace_back(std::move(temp));
    out.emplace_back(std::move(out_temp));
};

std::vector<Point> PrintObject::get_instances_shift_without_plate_offset()
{
    std::vector<Point> out;
    out.reserve(m_instances.size());
    for (const auto& instance : m_instances)
        out.push_back(instance.shift_without_plate_offset());

    return out;
}

// Only active if config->infill_only_where_needed. This step trims the sparse infill,
// so it acts as an internal support. It maintains all other infill types intact.
// Here the internal surfaces and perimeters have to be supported by the sparse infill.
//FIXME The surfaces are supported by a sparse infill, but the sparse infill is only as large as the area to support.
// Likely the sparse infill will not be anchored correctly, so it will not work as intended.
// Also one wishes the perimeters to be supported by a full infill.
// Idempotence of this method is guaranteed by the fact that we don't remove things from
// fill_surfaces but we only turn them into VOID surfaces, thus preserving the boundaries.
void PrintObject::clip_fill_surfaces()
{
    if (! PrintObject::infill_only_where_needed)
        return;
    bool has_infill = false;
    for (size_t i = 0; i < this->num_printing_regions(); ++ i)
        if (this->printing_region(i).config().sparse_infill_density > 0) {
            has_infill = true;
            break;
        }
    if (! has_infill)
        return;

    // We only want infill under ceilings; this is almost like an
    // internal support material.
    // Proceed top-down, skipping the bottom layer.
    Polygons upper_internal;
    for (int layer_id = int(m_layers.size()) - 1; layer_id > 0; -- layer_id) {
        Layer *layer       = m_layers[layer_id];
        Layer *lower_layer = m_layers[layer_id - 1];
        // Detect things that we need to support.
        // Cummulative fill surfaces.
        Polygons fill_surfaces;
        // Solid surfaces to be supported.
        Polygons overhangs;
        for (const LayerRegion *layerm : layer->m_regions)
            for (const Surface &surface : layerm->fill_surfaces.surfaces) {
                Polygons polygons = to_polygons(surface.expolygon);
                if (surface.is_solid())
                    polygons_append(overhangs, polygons);
                polygons_append(fill_surfaces, std::move(polygons));
            }
        Polygons lower_layer_fill_surfaces;
        Polygons lower_layer_internal_surfaces;
        for (const LayerRegion *layerm : lower_layer->m_regions)
            for (const Surface &surface : layerm->fill_surfaces.surfaces) {
                Polygons polygons = to_polygons(surface.expolygon);
                if (surface.surface_type == stInternal || surface.surface_type == stInternalVoid)
                    polygons_append(lower_layer_internal_surfaces, polygons);
                polygons_append(lower_layer_fill_surfaces, std::move(polygons));
            }
        // We also need to support perimeters when there's at least one full unsupported loop
        {
            // Get perimeters area as the difference between slices and fill_surfaces
            // Only consider the area that is not supported by lower perimeters
            Polygons perimeters = intersection(diff(layer->lslices, fill_surfaces), lower_layer_fill_surfaces);
            // Only consider perimeter areas that are at least one extrusion width thick.
            //FIXME Offset2 eats out from both sides, while the perimeters are create outside in.
            //Should the pw not be half of the current value?
            float pw = FLT_MAX;
            for (const LayerRegion *layerm : layer->m_regions)
                pw = std::min(pw, (float)layerm->flow(frPerimeter).scaled_width());
            // Append such thick perimeters to the areas that need support
            polygons_append(overhangs, opening(perimeters, pw));
        }
        // Merge the new overhangs, find new internal infill.
        polygons_append(upper_internal, std::move(overhangs));
        static constexpr const auto closing_radius = scaled<float>(2.f);
        upper_internal = intersection(
            // Regularize the overhang regions, so that the infill areas will not become excessively jagged.
            smooth_outward(
                closing(upper_internal, closing_radius, ClipperLib::jtSquare, 0.),
                scaled<coord_t>(0.1)),
            lower_layer_internal_surfaces);
        // Apply new internal infill to regions.
        for (LayerRegion *layerm : lower_layer->m_regions) {
            if (layerm->region().config().sparse_infill_density.value == 0)
                continue;
            SurfaceType internal_surface_types[] = { stInternal, stInternalVoid };
            Polygons internal;
            for (Surface &surface : layerm->fill_surfaces.surfaces)
                if (surface.surface_type == stInternal || surface.surface_type == stInternalVoid)
                    polygons_append(internal, std::move(surface.expolygon));
            layerm->fill_surfaces.remove_types(internal_surface_types, 2);
            layerm->fill_surfaces.append(intersection_ex(internal, upper_internal, ApplySafetyOffset::Yes), stInternal);
            layerm->fill_surfaces.append(diff_ex        (internal, upper_internal, ApplySafetyOffset::Yes), stInternalVoid);
            // If there are voids it means that our internal infill is not adjacent to
            // perimeters. In this case it would be nice to add a loop around infill to
            // make it more robust and nicer. TODO.
#ifdef SLIC3R_DEBUG_SLICE_PROCESSING
            layerm->export_region_fill_surfaces_to_svg_debug("6_clip_fill_surfaces");
#endif
        }
        m_print->throw_if_canceled();
    }
}

void PrintObject::discover_horizontal_shells()
{
    BOOST_LOG_TRIVIAL(trace) << "discover_horizontal_shells()";

    for (size_t region_id = 0; region_id < this->num_printing_regions(); ++ region_id) {
        for (size_t i = 0; i < m_layers.size(); ++ i) {
            m_print->throw_if_canceled();
            Layer 					*layer  = m_layers[i];
            LayerRegion             *layerm = layer->regions()[region_id];
            const PrintRegionConfig &region_config = layerm->region().config();
#if 0
            if (region_config.solid_infill_every_layers.value > 0 && region_config.sparse_infill_density.value > 0 &&
                (i % region_config.solid_infill_every_layers) == 0) {
                // Insert a solid internal layer. Mark stInternal surfaces as stInternalSolid or stInternalBridge.
                SurfaceType type = (region_config.sparse_infill_density == 100 || region_config.solid_infill_every_layers == 1) ? stInternalSolid : stInternalBridge;
                for (Surface &surface : layerm->fill_surfaces.surfaces)
                    if (surface.surface_type == stInternal)
                        surface.surface_type = type;
            }
#endif

            // If ensure_vertical_shell_thickness, then the rest has already been performed by discover_vertical_shells().
            if (region_config.ensure_vertical_shell_thickness.value)
                continue;

            coordf_t print_z  = layer->print_z;
            coordf_t bottom_z = layer->bottom_z();
            for (size_t idx_surface_type = 0; idx_surface_type < 3; ++ idx_surface_type) {
                m_print->throw_if_canceled();
                SurfaceType type = (idx_surface_type == 0) ? stTop : (idx_surface_type == 1) ? stBottom : stBottomBridge;
                int num_solid_layers = (type == stTop) ? region_config.top_shell_layers.value : region_config.bottom_shell_layers.value;
                if (num_solid_layers == 0)
                	continue;
                // Find slices of current type for current layer.
                // Use slices instead of fill_surfaces, because they also include the perimeter area,
                // which needs to be propagated in shells; we need to grow slices like we did for
                // fill_surfaces though. Using both ungrown slices and grown fill_surfaces will
                // not work in some situations, as there won't be any grown region in the perimeter
                // area (this was seen in a model where the top layer had one extra perimeter, thus
                // its fill_surfaces were thinner than the lower layer's infill), however it's the best
                // solution so far. Growing the external slices by EXTERNAL_INFILL_MARGIN will put
                // too much solid infill inside nearly-vertical slopes.

                // Surfaces including the area of perimeters. Everything, that is visible from the top / bottom
                // (not covered by a layer above / below).
                // This does not contain the areas covered by perimeters!
                Polygons solid;
                for (const Surface &surface : layerm->slices.surfaces)
                    if (surface.surface_type == type)
                        polygons_append(solid, to_polygons(surface.expolygon));
                // Infill areas (slices without the perimeters).
                for (const Surface &surface : layerm->fill_surfaces.surfaces)
                    if (surface.surface_type == type)
                        polygons_append(solid, to_polygons(surface.expolygon));
                if (solid.empty())
                    continue;
//                Slic3r::debugf "Layer %d has %s surfaces\n", $i, ($type == stTop) ? 'top' : 'bottom';

                // Scatter top / bottom regions to other layers. Scattering process is inherently serial, it is difficult to parallelize without locking.
                for (int n = (type == stTop) ? int(i) - 1 : int(i) + 1;
                	(type == stTop) ?
                		(n >= 0                   && (int(i) - n < num_solid_layers ||
                								 	  print_z - m_layers[n]->print_z < region_config.top_shell_thickness.value - EPSILON)) :
                		(n < int(m_layers.size()) && (n - int(i) < num_solid_layers ||
                									  m_layers[n]->bottom_z() - bottom_z < region_config.bottom_shell_thickness.value - EPSILON));
                	(type == stTop) ? -- n : ++ n)
                {
//                    Slic3r::debugf "  looking for neighbors on layer %d...\n", $n;
                    // Reference to the lower layer of a TOP surface, or an upper layer of a BOTTOM surface.
                    LayerRegion *neighbor_layerm = m_layers[n]->regions()[region_id];

                    // find intersection between neighbor and current layer's surfaces
                    // intersections have contours and holes
                    // we update $solid so that we limit the next neighbor layer to the areas that were
                    // found on this one - in other words, solid shells on one layer (for a given external surface)
                    // are always a subset of the shells found on the previous shell layer
                    // this approach allows for DWIM in hollow sloping vases, where we want bottom
                    // shells to be generated in the base but not in the walls (where there are many
                    // narrow bottom surfaces): reassigning $solid will consider the 'shadow' of the
                    // upper perimeter as an obstacle and shell will not be propagated to more upper layers
                    //FIXME How does it work for stInternalBRIDGE? This is set for sparse infill. Likely this does not work.
                    Polygons new_internal_solid;
                    {
                        Polygons internal;
                        for (const Surface &surface : neighbor_layerm->fill_surfaces.surfaces)
                            if (surface.surface_type == stInternal || surface.surface_type == stInternalSolid)
                                polygons_append(internal, to_polygons(surface.expolygon));
                        new_internal_solid = intersection(solid, internal, ApplySafetyOffset::Yes);
                    }
                    if (new_internal_solid.empty()) {
                        // No internal solid needed on this layer. In order to decide whether to continue
                        // searching on the next neighbor (thus enforcing the configured number of solid
                        // layers, use different strategies according to configured infill density:
                        if (region_config.sparse_infill_density.value == 0) {
                            // If user expects the object to be void (for example a hollow sloping vase),
                            // don't continue the search. In this case, we only generate the external solid
                            // shell if the object would otherwise show a hole (gap between perimeters of
                            // the two layers), and internal solid shells are a subset of the shells found
                            // on each previous layer.
                            goto EXTERNAL;
                        } else {
                            // If we have internal infill, we can generate internal solid shells freely.
                            continue;
                        }
                    }

                    if (region_config.sparse_infill_density.value == 0) {
                        // if we're printing a hollow object we discard any solid shell thinner
                        // than a perimeter width, since it's probably just crossing a sloping wall
                        // and it's not wanted in a hollow print even if it would make sense when
                        // obeying the solid shell count option strictly (DWIM!)
                        float margin = float(neighbor_layerm->flow(frExternalPerimeter).scaled_width());
                        Polygons too_narrow = diff(
                            new_internal_solid,
                            opening(new_internal_solid, margin, margin + ClipperSafetyOffset, jtMiter, 5));
                        // Trim the regularized region by the original region.
                        if (! too_narrow.empty())
                            new_internal_solid = solid = diff(new_internal_solid, too_narrow);
                    }

                    // make sure the new internal solid is wide enough, as it might get collapsed
                    // when spacing is added in Fill.pm
                    {
                        //FIXME Vojtech: Disable this and you will be sorry.
                        float margin = 3.f * layerm->flow(frSolidInfill).scaled_width(); // require at least this size
                        // we use a higher miterLimit here to handle areas with acute angles
                        // in those cases, the default miterLimit would cut the corner and we'd
                        // get a triangle in $too_narrow; if we grow it below then the shell
                        // would have a different shape from the external surface and we'd still
                        // have the same angle, so the next shell would be grown even more and so on.
                        Polygons too_narrow = diff(
                            new_internal_solid,
                            opening(new_internal_solid, margin, margin + ClipperSafetyOffset, ClipperLib::jtMiter, 5));
                        if (! too_narrow.empty()) {
                            // grow the collapsing parts and add the extra area to  the neighbor layer
                            // as well as to our original surfaces so that we support this
                            // additional area in the next shell too
                            // make sure our grown surfaces don't exceed the fill area
                            Polygons internal;
                            for (const Surface &surface : neighbor_layerm->fill_surfaces.surfaces)
                                if (surface.is_internal() && !surface.is_bridge())
                                    polygons_append(internal, to_polygons(surface.expolygon));
                            polygons_append(new_internal_solid,
                                intersection(
                                    expand(too_narrow, +margin),
                                    // Discard bridges as they are grown for anchoring and we can't
                                    // remove such anchors. (This may happen when a bridge is being
                                    // anchored onto a wall where little space remains after the bridge
                                    // is grown, and that little space is an internal solid shell so
                                    // it triggers this too_narrow logic.)
                                    internal));
                            // solid = new_internal_solid;
                        }
                    }

                    // internal-solid are the union of the existing internal-solid surfaces
                    // and new ones
                    SurfaceCollection backup = std::move(neighbor_layerm->fill_surfaces);
                    polygons_append(new_internal_solid, to_polygons(backup.filter_by_type(stInternalSolid)));
                    ExPolygons internal_solid = union_ex(new_internal_solid);
                    // assign new internal-solid surfaces to layer
                    neighbor_layerm->fill_surfaces.set(internal_solid, stInternalSolid);
                    // subtract intersections from layer surfaces to get resulting internal surfaces
                    Polygons polygons_internal = to_polygons(std::move(internal_solid));
                    ExPolygons internal = diff_ex(backup.filter_by_type(stInternal), polygons_internal, ApplySafetyOffset::Yes);
                    // assign resulting internal surfaces to layer
                    neighbor_layerm->fill_surfaces.append(internal, stInternal);
                    polygons_append(polygons_internal, to_polygons(std::move(internal)));
                    // assign top and bottom surfaces to layer
                    SurfaceType surface_types_solid[] = { stTop, stBottom, stBottomBridge };
                    backup.keep_types(surface_types_solid, 3);
                    std::vector<SurfacesPtr> top_bottom_groups;
                    backup.group(&top_bottom_groups);
                    for (SurfacesPtr &group : top_bottom_groups)
                        neighbor_layerm->fill_surfaces.append(
                            diff_ex(group, polygons_internal),
                            // Use an existing surface as a template, it carries the bridge angle etc.
                            *group.front());
                }
		EXTERNAL:;
            } // foreach type (stTop, stBottom, stBottomBridge)
        } // for each layer
    } // for each region

#ifdef SLIC3R_DEBUG_SLICE_PROCESSING
    for (size_t region_id = 0; region_id < this->num_printing_regions(); ++ region_id) {
        for (const Layer *layer : m_layers) {
            const LayerRegion *layerm = layer->m_regions[region_id];
            layerm->export_region_slices_to_svg_debug("5_discover_horizontal_shells");
            layerm->export_region_fill_surfaces_to_svg_debug("5_discover_horizontal_shells");
        } // for each layer
    } // for each region
#endif /* SLIC3R_DEBUG_SLICE_PROCESSING */
}

// combine fill surfaces across layers to honor the "infill every N layers" option
// Idempotence of this method is guaranteed by the fact that we don't remove things from
// fill_surfaces but we only turn them into VOID surfaces, thus preserving the boundaries.
void PrintObject::combine_infill()
{
    // Work on each region separately.
    for (size_t region_id = 0; region_id < this->num_printing_regions(); ++ region_id) {
        const PrintRegion &region = this->printing_region(region_id);
        //BBS
        const bool enable_combine_infill = region.config().infill_combination.value;
        if (enable_combine_infill == false || region.config().sparse_infill_density == 0.)
            continue;
        // Limit the number of combined layers to the maximum height allowed by this regions' nozzle.
        //FIXME limit the layer height to max_layer_height
        double nozzle_diameter = std::min(
            this->print()->config().nozzle_diameter.get_at(region.config().sparse_infill_filament.value - 1),
            this->print()->config().nozzle_diameter.get_at(region.config().solid_infill_filament.value - 1));
        // define the combinations
        std::vector<size_t> combine(m_layers.size(), 0);
        {
            double current_height = 0.;
            size_t num_layers = 0;
            for (size_t layer_idx = 0; layer_idx < m_layers.size(); ++ layer_idx) {
                m_print->throw_if_canceled();
                const Layer *layer = m_layers[layer_idx];
                if (layer->id() == 0)
                    // Skip first print layer (which may not be first layer in array because of raft).
                    continue;
                // Check whether the combination of this layer with the lower layers' buffer
                // would exceed max layer height or max combined layer count.
                // BBS: automatically calculate how many layers should be combined
                if (current_height + layer->height >= nozzle_diameter + EPSILON) {
                    // Append combination to lower layer.
                    combine[layer_idx - 1] = num_layers;
                    current_height = 0.;
                    num_layers = 0;
                }
                current_height += layer->height;
                ++ num_layers;
            }

            // Append lower layers (if any) to uppermost layer.
            combine[m_layers.size() - 1] = num_layers;
        }

        // loop through layers to which we have assigned layers to combine
        for (size_t layer_idx = 0; layer_idx < m_layers.size(); ++ layer_idx) {
            m_print->throw_if_canceled();
            size_t num_layers = combine[layer_idx];
			if (num_layers <= 1)
                continue;
            // Get all the LayerRegion objects to be combined.
            std::vector<LayerRegion*> layerms;
            layerms.reserve(num_layers);
			for (size_t i = layer_idx + 1 - num_layers; i <= layer_idx; ++ i)
                layerms.emplace_back(m_layers[i]->regions()[region_id]);
            // We need to perform a multi-layer intersection, so let's split it in pairs.
            // Initialize the intersection with the candidates of the lowest layer.
            ExPolygons intersection = to_expolygons(layerms.front()->fill_surfaces.filter_by_type(stInternal));
            // Start looping from the second layer and intersect the current intersection with it.
            for (size_t i = 1; i < layerms.size(); ++ i)
                intersection = intersection_ex(layerms[i]->fill_surfaces.filter_by_type(stInternal), intersection);
            double area_threshold = layerms.front()->infill_area_threshold();
            if (! intersection.empty() && area_threshold > 0.)
                intersection.erase(std::remove_if(intersection.begin(), intersection.end(),
                    [area_threshold](const ExPolygon &expoly) { return expoly.area() <= area_threshold; }),
                    intersection.end());
            if (intersection.empty())
                continue;
//            Slic3r::debugf "  combining %d %s regions from layers %d-%d\n",
//                scalar(@$intersection),
//                ($type == stInternal ? 'internal' : 'internal-solid'),
//                $layer_idx-($every-1), $layer_idx;
            // intersection now contains the regions that can be combined across the full amount of layers,
            // so let's remove those areas from all layers.
            Polygons intersection_with_clearance;
            intersection_with_clearance.reserve(intersection.size());
            float clearance_offset =
                0.5f * layerms.back()->flow(frPerimeter).scaled_width() +
             // Because fill areas for rectilinear and honeycomb are grown
             // later to overlap perimeters, we need to counteract that too.
                ((region.config().sparse_infill_pattern == ipRectilinear   ||
                  region.config().sparse_infill_pattern == ipMonotonic     ||
                  region.config().sparse_infill_pattern == ipGrid          ||
                  region.config().sparse_infill_pattern == ipLine          ||
                  region.config().sparse_infill_pattern == ipHoneycomb) ? 1.5f : 0.5f) *
                    layerms.back()->flow(frSolidInfill).scaled_width();
            for (ExPolygon &expoly : intersection)
                polygons_append(intersection_with_clearance, offset(expoly, clearance_offset));
            for (LayerRegion *layerm : layerms) {
                Polygons internal = to_polygons(std::move(layerm->fill_surfaces.filter_by_type(stInternal)));
                layerm->fill_surfaces.remove_type(stInternal);
                layerm->fill_surfaces.append(diff_ex(internal, intersection_with_clearance), stInternal);
                if (layerm == layerms.back()) {
                    // Apply surfaces back with adjusted depth to the uppermost layer.
                    Surface templ(stInternal, ExPolygon());
                    templ.thickness = 0.;
                    for (LayerRegion *layerm2 : layerms)
                        templ.thickness += layerm2->layer()->height;
                    templ.thickness_layers = (unsigned short)layerms.size();
                    layerm->fill_surfaces.append(intersection, templ);
                } else {
                    // Save void surfaces.
                    layerm->fill_surfaces.append(
                        intersection_ex(internal, intersection_with_clearance),
                        stInternalVoid);
                }
            }
        }
    }
}

void PrintObject::_generate_support_material()
{
    PrintObjectSupportMaterial support_material(this, m_slicing_params);
    support_material.generate(*this);

    if (this->config().enable_support.value && is_tree(this->config().support_type.value)) {
        if (this->config().support_style.value == smsOrganic || this->config().support_style.value == smsDefault) {
            fff_tree_support_generate(*this, std::function<void()>([this]() { this->throw_if_canceled(); }));
        } else {
            TreeSupport tree_support(*this, m_slicing_params);
            tree_support.generate();
        }
    }
}

// BBS
#define SUPPORT_SURFACES_OFFSET_PARAMETERS ClipperLib::jtSquare, 0.
#define SUPPORT_MATERIAL_MARGIN 1.2
template<typename PolysType>
void PrintObject::remove_bridges_from_contacts(
    const Layer* lower_layer,
    const Layer* current_layer,
    float extrusion_width,
    PolysType* overhang_regions,
    float max_bridge_length,
    bool break_bridge)
{
    // Extrusion width accounts for the roundings of the extrudates.
    // It is the maximum widh of the extrudate.
    float fw = extrusion_width;
    Lines overhang_perimeters = to_lines(*overhang_regions);
    auto layer_regions = current_layer->regions();
    Polygons lower_layer_polygons = to_polygons(lower_layer->lslices);
    const PrintObjectConfig& object_config = current_layer->object()->config();

    Polygons all_bridges;
    for (LayerRegion* layerm : layer_regions)
    {
        Polygons bridges;
        // Surface supporting this layer, expanded by 0.5 * nozzle_diameter, as we consider this kind of overhang to be sufficiently supported.
        Polygons lower_grown_slices = offset(lower_layer_polygons,
            //FIXME to mimic the decision in the perimeter generator, we should use half the external perimeter width.
            0.5f * fw, SUPPORT_SURFACES_OFFSET_PARAMETERS);
        Polylines overhang_perimeters = diff_pl(layerm->perimeters.as_polylines(), lower_grown_slices);
        // only consider straight overhangs
            // only consider overhangs having endpoints inside layer's slices
            // convert bridging polylines into polygons by inflating them with their thickness
            // since we're dealing with bridges, we can't assume width is larger than spacing,
            // so we take the largest value and also apply safety offset to be ensure no gaps
            // are left in between
        Flow bridge_flow = layerm->bridging_flow(frPerimeter, object_config.thick_bridges);
        float w = float(std::max(bridge_flow.scaled_width(), bridge_flow.scaled_spacing()));
        for (Polyline& polyline : overhang_perimeters)
            if (polyline.is_straight()) {
                // This is a bridge
                polyline.extend_start(fw);
                polyline.extend_end(fw);
                // Is the straight perimeter segment supported at both sides?
                Point pts[2] = { polyline.first_point(), polyline.last_point() };
                bool  supported[2] = { false, false };
                for (size_t i = 0; i < lower_layer->lslices.size() && !(supported[0] && supported[1]); ++i)
                    for (int j = 0; j < 2; ++j)
                        if (!supported[j] && lower_layer->lslices_bboxes[i].contains(pts[j]) && lower_layer->lslices[i].contains(pts[j]))
                            supported[j] = true;
                if (supported[0] && supported[1]) {
                    Polylines lines;
                    if (polyline.length() > max_bridge_length + 10) {
                        if (break_bridge) {
                            // equally divide the polyline
                            float len = polyline.length() / ceil(polyline.length() / max_bridge_length);
                            lines = polyline.equally_spaced_lines(len);
                            for (auto& line : lines) {
                                if (line.is_valid())
                                    line.clip_start(fw);
                                if (line.is_valid())
                                    line.clip_end(fw);
                            }
                        }
                    }
                    else
                        lines.push_back(polyline);
                    // Offset a polyline into a thick line.
                    polygons_append(bridges, offset(lines, 0.5f * w + 10.f));
                }
            }
        bridges = union_(bridges);

        // remove the entire bridges and only support the unsupported edges
        //FIXME the brided regions are already collected as layerm->bridged. Use it?
        for (const Surface& surface : layerm->fill_surfaces.surfaces)
            if (surface.surface_type == stBottomBridge && surface.bridge_angle != -1) {
                auto bbox      = get_extents(surface.expolygon);
                auto bbox_size = bbox.size();
                if (bbox_size[0] < max_bridge_length && bbox_size[1] < max_bridge_length)
                    polygons_append(bridges, surface.expolygon);
                else {
                    if (break_bridge) {
                        Polygons holes;
                        coord_t  x0 = bbox.min.x();
                        coord_t  x1 = bbox.max.x();
                        coord_t  y0 = bbox.min.y();
                        coord_t  y1 = bbox.max.y();
                        const int grid_lw = int(w/2); // grid line width

                        Vec2f bridge_direction{ cos(surface.bridge_angle),sin(surface.bridge_angle) };
                        if (fabs(bridge_direction(0)) > fabs(bridge_direction(1)))
                        {   // cut bridge along x-axis if bridge direction is aligned to x-axis more than to y-axis
                            // Note: surface.bridge_angle may be pi, so we can't compare it to 0 & pi/2.
                            int step = bbox_size(0) / ceil(bbox_size(0) / max_bridge_length);
                            for (int x = x0 + step; x < x1; x += step) {
                                Polygon poly;
                                poly.points = {Point(x - grid_lw, y0), Point(x + grid_lw, y0), Point(x + grid_lw, y1), Point(x - grid_lw, y1)};
                                holes.emplace_back(poly);
                            }
                        } else {
                            int step = bbox_size(1) / ceil(bbox_size(1) / max_bridge_length);
                            for (int y = y0 + step; y < y1; y += step) {
                                Polygon poly;
                                poly.points = {Point(x0, y - grid_lw), Point(x0, y + grid_lw), Point(x1, y + grid_lw), Point(x1, y - grid_lw)};
                                holes.emplace_back(poly);
                            }
                        }
                        auto expoly = diff_ex(surface.expolygon, holes);
                        polygons_append(bridges, expoly);
                    }
                }
            }
        //FIXME add the gap filled areas. Extrude the gaps with a bridge flow?
        // Remove the unsupported ends of the bridges from the bridged areas.
        //FIXME add supports at regular intervals to support long bridges!
        bridges = diff(bridges,
            // Offset unsupported edges into polygons.
            offset(layerm->unsupported_bridge_edges, scale_(SUPPORT_MATERIAL_MARGIN), SUPPORT_SURFACES_OFFSET_PARAMETERS));
        append(all_bridges, bridges);
    }
    if (typeid(overhang_regions) == typeid(ExPolygons*)) {
        *(ExPolygons*)overhang_regions = diff_ex(*overhang_regions, all_bridges, ApplySafetyOffset::Yes);
    }
    else if (typeid(overhang_regions) == typeid(Polygons*)) {
        *(Polygons*)overhang_regions = diff(*overhang_regions, all_bridges, ApplySafetyOffset::Yes);
    }
}

template void PrintObject::remove_bridges_from_contacts<ExPolygons>(
    const Layer* lower_layer,
    const Layer* current_layer,
    float extrusion_width,
    ExPolygons* overhang_regions,
    float max_bridge_length, bool break_bridge);
template void PrintObject::remove_bridges_from_contacts<Polygons>(
    const Layer* lower_layer,
    const Layer* current_layer,
    float extrusion_width,
    Polygons* overhang_regions,
    float max_bridge_length, bool break_bridge);


SupportNecessaryType PrintObject::is_support_necessary()
{
    static const double super_overhang_area_threshold = SQ(scale_(5.0));
    const double cantilevel_dist_thresh = scale_(6);
#if 0
    double threshold_rad = (m_config.support_threshold_angle.value < EPSILON ? 30 : m_config.support_threshold_angle.value + 1) * M_PI / 180.;
    int enforce_support_layers = m_config.enforce_support_layers;
    // not fixing in extrusion width % PR b/c never called 
    const coordf_t extrusion_width = m_config.line_width.value;
    const coordf_t extrusion_width_scaled = scale_(extrusion_width);
    float max_bridge_length = scale_(m_config.max_bridge_length.value);
    const bool bridge_no_support = max_bridge_length > 0;// config.bridge_no_support.value;

    for (size_t layer_nr = enforce_support_layers + 1; layer_nr < this->layer_count(); layer_nr++) {
        Layer* layer = m_layers[layer_nr];
        Layer* lower_layer = layer->lower_layer;

        coordf_t support_offset_scaled = extrusion_width_scaled * 0.9;
        ExPolygons lower_layer_offseted = offset_ex(lower_layer->lslices, support_offset_scaled, SUPPORT_SURFACES_OFFSET_PARAMETERS);

        // 1. check sharp tail
        for (const LayerRegion* layerm : layer->regions()) {
            for (const ExPolygon& expoly : layerm->raw_slices) {
                // detect sharp tail
                if (intersection_ex({ expoly }, lower_layer_offseted).empty())
                    return SharpTail;
            }
        }

        // 2. check overhang area
        ExPolygons super_overhang_expolys = std::move(diff_ex(layer->lslices, lower_layer_offseted));
        super_overhang_expolys.erase(std::remove_if(
            super_overhang_expolys.begin(),
            super_overhang_expolys.end(),
            [extrusion_width_scaled](ExPolygon& area) {
                return offset_ex(area, -0.1 * extrusion_width_scaled).empty();
            }),
            super_overhang_expolys.end());

        // remove bridge
        if (bridge_no_support)
            remove_bridges_from_contacts(lower_layer, layer, extrusion_width_scaled, &super_overhang_expolys, max_bridge_length);

        Polygons super_overhang_polys = to_polygons(super_overhang_expolys);


        super_overhang_polys.erase(std::remove_if(
            super_overhang_polys.begin(),
            super_overhang_polys.end(),
            [extrusion_width_scaled](Polygon& area) {
                return offset_ex(area, -0.1 * extrusion_width_scaled).empty();
            }),
            super_overhang_polys.end());

        double super_overhang_area = 0.0;
        for (Polygon& poly : super_overhang_polys) {
            bool is_ccw = poly.is_counter_clockwise();
            double area_  = poly.area();
            if (is_ccw) {
                if (area_ > super_overhang_area_threshold)
                    return LargeOverhang;
                super_overhang_area += area_;
            }
            else {
                super_overhang_area -= area_;
            }
        }

        //if (super_overhang_area > super_overhang_area_threshold)
        //    return LargeOverhang;

        // 3. check overhang distance
        const double distance_threshold_scaled = extrusion_width_scaled * 2;
        ExPolygons lower_layer_offseted_2 = offset_ex(lower_layer->lslices, distance_threshold_scaled, SUPPORT_SURFACES_OFFSET_PARAMETERS);
        ExPolygons exceed_overhang = std::move(diff_ex(super_overhang_polys, lower_layer_offseted_2));
        exceed_overhang.erase(std::remove_if(
            exceed_overhang.begin(),
            exceed_overhang.end(),
            [extrusion_width_scaled](ExPolygon& area) {
                // tolerance for 1 extrusion width offset
                return offset_ex(area, -0.5 * extrusion_width_scaled).empty();
            }),
            exceed_overhang.end());
        if (!exceed_overhang.empty())
            return LargeOverhang;
    }
#else
    TreeSupport tree_support(*this, m_slicing_params);
    tree_support.support_type = SupportType::stTreeAuto; // need to set support type to fully utilize the power of feature detection
    tree_support.detect_overhangs(true);
    this->clear_support_layers();
    if (tree_support.has_sharp_tails)
        return SharpTail;
    else if (tree_support.has_cantilever && tree_support.max_cantilever_dist > cantilevel_dist_thresh)
        return Cantilever;
#endif
    return NoNeedSupp;
}

static void project_triangles_to_slabs(ConstLayerPtrsAdaptor layers, const indexed_triangle_set &custom_facets, const Transform3f &tr, bool seam, std::vector<Polygons> &out)
{
    if (custom_facets.indices.empty())
        return;

    const float tr_det_sign = (tr.matrix().determinant() > 0. ? 1.f : -1.f);

    // The projection will be at most a pentagon. Let's minimize heap
    // reallocations by saving in in the following struct.
    // Points are used so that scaling can be done in parallel
    // and they can be moved from to create an ExPolygon later.
    struct LightPolygon {
        LightPolygon() { pts.reserve(5); }
        LightPolygon(const std::array<Vec2f, 3>& tri) {
            pts.reserve(3);
            pts.emplace_back(scaled<coord_t>(tri.front()));
            pts.emplace_back(scaled<coord_t>(tri[1]));
            pts.emplace_back(scaled<coord_t>(tri.back()));
        }

        Points pts;

        void add(const Vec2f& pt) {
            pts.emplace_back(scaled<coord_t>(pt));
            assert(pts.size() <= 5);
        }
    };

    // Structure to collect projected polygons. One element for each triangle.
    // Saves vector of polygons and layer_id of the first one.
    struct TriangleProjections {
        size_t first_layer_id;
        std::vector<LightPolygon> polygons;
    };

    // Vector to collect resulting projections from each triangle.
    std::vector<TriangleProjections> projections_of_triangles(custom_facets.indices.size());

    // Iterate over all triangles.
    tbb::parallel_for(
        tbb::blocked_range<size_t>(0, custom_facets.indices.size()),
        [&custom_facets, &tr, tr_det_sign, seam, layers, &projections_of_triangles](const tbb::blocked_range<size_t>& range) {
        for (size_t idx = range.begin(); idx < range.end(); ++ idx) {

        std::array<Vec3f, 3> facet;

        // Transform the triangle into worlds coords.
        for (int i=0; i<3; ++i)
            facet[i] = tr * custom_facets.vertices[custom_facets.indices[idx](i)];

        // Ignore triangles with upward-pointing normal. Don't forget about mirroring.
        float z_comp = (facet[1]-facet[0]).cross(facet[2]-facet[0]).z();
        if (! seam && tr_det_sign * z_comp > 0.)
            continue;

        // The algorithm does not process vertical triangles, but it should for seam.
        // In that case, tilt the triangle a bit so the projection does not degenerate.
        if (seam && z_comp == 0.f)
            facet[0].x() += float(EPSILON);

        // Sort the three vertices according to z-coordinate.
        std::sort(facet.begin(), facet.end(),
                  [](const Vec3f& pt1, const Vec3f&pt2) {
                      return pt1.z() < pt2.z();
                  });

        std::array<Vec2f, 3> trianglef;
        for (int i=0; i<3; ++i)
            trianglef[i] = to_2d(facet[i]);

        // Find lowest slice not below the triangle.
        auto it = std::lower_bound(layers.begin(), layers.end(), facet[0].z()+EPSILON,
                      [](const Layer* l1, float z) {
                           return l1->slice_z < z;
                      });

        // Count how many projections will be generated for this triangle
        // and allocate respective amount in projections_of_triangles.
        size_t first_layer_id = projections_of_triangles[idx].first_layer_id = it - layers.begin();
        size_t last_layer_id  = first_layer_id;
        // The cast in the condition below is important. The comparison must
        // be an exact opposite of the one lower in the code where
        // the polygons are appended. And that one is on floats.
        while (last_layer_id + 1 < layers.size()
            && float(layers[last_layer_id]->slice_z) <= facet[2].z())
            ++last_layer_id;

        if (first_layer_id == last_layer_id) {
            // The triangle fits just a single slab, just project it. This also avoids division by zero for horizontal triangles.
            float dz = facet[2].z() - facet[0].z();
            assert(dz >= 0);
            // The face is nearly horizontal and it crosses the slicing plane at first_layer_id - 1.
            // Rather add this face to both the planes.
            bool add_below = dz < float(2. * EPSILON) && first_layer_id > 0 && layers[first_layer_id - 1]->slice_z > facet[0].z() - EPSILON;
            projections_of_triangles[idx].polygons.reserve(add_below ? 2 : 1);
            projections_of_triangles[idx].polygons.emplace_back(trianglef);
            if (add_below) {
                -- projections_of_triangles[idx].first_layer_id;
                projections_of_triangles[idx].polygons.emplace_back(trianglef);
            }
            continue;
        }

        projections_of_triangles[idx].polygons.resize(last_layer_id - first_layer_id + 1);

        // Calculate how to move points on triangle sides per unit z increment.
        Vec2f ta(trianglef[1] - trianglef[0]);
        Vec2f tb(trianglef[2] - trianglef[0]);
        ta *= 1.f/(facet[1].z() - facet[0].z());
        tb *= 1.f/(facet[2].z() - facet[0].z());

        // Projection on current slice will be built directly in place.
        LightPolygon* proj = &projections_of_triangles[idx].polygons[0];
        proj->add(trianglef[0]);

        bool passed_first = false;
        bool stop = false;

        // Project a sub-polygon on all slices intersecting the triangle.
        while (it != layers.end()) {
            const float z = float((*it)->slice_z);

            // Projections of triangle sides intersections with slices.
            // a moves along one side, b tracks the other.
            Vec2f a;
            Vec2f b;

            // If the middle vertex was already passed, append the vertex
            // and use ta for tracking the remaining side.
            if (z > facet[1].z() && ! passed_first) {
                proj->add(trianglef[1]);
                ta = trianglef[2]-trianglef[1];
                ta *= 1.f/(facet[2].z() - facet[1].z());
                passed_first = true;
            }

            // This slice is above the triangle already.
            if (z > facet[2].z() || it+1 == layers.end()) {
                proj->add(trianglef[2]);
                stop = true;
            }
            else {
                // Move a, b along the side it currently tracks to get
                // projected intersection with current slice.
                a = passed_first ? (trianglef[1]+ta*(z-facet[1].z()))
                                 : (trianglef[0]+ta*(z-facet[0].z()));
                b = trianglef[0]+tb*(z-facet[0].z());
                proj->add(a);
                proj->add(b);
            }

           if (stop)
                break;

            // Advance to the next layer.
            ++it;
            ++proj;
            assert(proj <= &projections_of_triangles[idx].polygons.back() );

            // a, b are first two points of the polygon for the next layer.
            proj->add(b);
            proj->add(a);
        }
    }
    }); // end of parallel_for

    // Make sure that the output vector can be used.
    out.resize(layers.size());

    // Now append the collected polygons to respective layers.
    for (auto& trg : projections_of_triangles) {
        int layer_id = int(trg.first_layer_id);
        for (LightPolygon &poly : trg.polygons) {
            if (layer_id >= int(out.size()))
                break; // part of triangle could be projected above top layer
            assert(! poly.pts.empty());
            // The resulting triangles are fed to the Clipper library, which seem to handle flipped triangles well.
//                if (cross2(Vec2d((poly.pts[1] - poly.pts[0]).cast<double>()), Vec2d((poly.pts[2] - poly.pts[1]).cast<double>())) < 0)
//                    std::swap(poly.pts.front(), poly.pts.back());

            out[layer_id].emplace_back(std::move(poly.pts));
            ++layer_id;
        }
    }
}

void PrintObject::project_and_append_custom_facets(
        bool seam, EnforcerBlockerType type, std::vector<Polygons>& out) const
{
    for (const ModelVolume* mv : this->model_object()->volumes)
        if (mv->is_model_part()) {
            const indexed_triangle_set custom_facets = seam
                    ? mv->seam_facets.get_facets_strict(*mv, type)
                    : mv->supported_facets.get_facets_strict(*mv, type);
            if (! custom_facets.indices.empty()) {
                if (seam)
                    project_triangles_to_slabs(this->layers(), custom_facets,
                        (this->trafo_centered() * mv->get_matrix()).cast<float>(),
                        seam, out);
                else {
                    std::vector<Polygons> projected;
                    // Support blockers or enforcers. Project downward facing painted areas upwards to their respective slicing plane.
                    slice_mesh_slabs(custom_facets, zs_from_layers(this->layers()), this->trafo_centered() * mv->get_matrix(), nullptr, &projected, [](){});
                    // Merge these projections with the output, layer by layer.
                    assert(! projected.empty());
                    assert(out.empty() || out.size() == projected.size());
                    if (out.empty())
                        out = std::move(projected);
                    else
                        for (size_t i = 0; i < out.size(); ++ i)
                            append(out[i], std::move(projected[i]));
                }
            }
        }
}

const Layer* PrintObject::get_layer_at_printz(coordf_t print_z) const {
    auto it = Slic3r::lower_bound_by_predicate(m_layers.begin(), m_layers.end(), [print_z](const Layer *layer) { return layer->print_z < print_z; });
    return (it == m_layers.end() || (*it)->print_z != print_z) ? nullptr : *it;
}



Layer* PrintObject::get_layer_at_printz(coordf_t print_z) { return const_cast<Layer*>(std::as_const(*this).get_layer_at_printz(print_z)); }



// Get a layer approximately at print_z.
const Layer* PrintObject::get_layer_at_printz(coordf_t print_z, coordf_t epsilon) const {
    coordf_t limit = print_z - epsilon;
    auto it = Slic3r::lower_bound_by_predicate(m_layers.begin(), m_layers.end(), [limit](const Layer *layer) { return layer->print_z < limit; });
    return (it == m_layers.end() || (*it)->print_z > print_z + epsilon) ? nullptr : *it;
}



Layer* PrintObject::get_layer_at_printz(coordf_t print_z, coordf_t epsilon) { return const_cast<Layer*>(std::as_const(*this).get_layer_at_printz(print_z, epsilon)); }

const Layer *PrintObject::get_first_layer_bellow_printz(coordf_t print_z, coordf_t epsilon) const
{
    coordf_t limit = print_z + epsilon;
    auto it = Slic3r::lower_bound_by_predicate(m_layers.begin(), m_layers.end(), [limit](const Layer *layer) { return layer->print_z < limit; });
    return (it == m_layers.begin()) ? nullptr : *(--it);
}
int PrintObject::get_layer_idx_get_printz(coordf_t print_z, coordf_t epsilon) {
    coordf_t limit = print_z + epsilon;
    auto     it    = Slic3r::lower_bound_by_predicate(m_layers.begin(), m_layers.end(), [limit](const Layer *layer) { return layer->print_z < limit; });
    return (it == m_layers.begin()) ? -1 : std::distance(m_layers.begin(), it);
}
// BBS
const Layer* PrintObject::get_layer_at_bottomz(coordf_t bottom_z, coordf_t epsilon) const {
    coordf_t limit_upper = bottom_z + epsilon;
    coordf_t limit_lower = bottom_z - epsilon;

    for (const Layer* layer : m_layers) {
        if (layer->bottom_z() > limit_lower)
            return layer->bottom_z() < limit_upper ? layer : nullptr;
    }

    return nullptr;
}

Layer* PrintObject::get_layer_at_bottomz(coordf_t bottom_z, coordf_t epsilon) { return const_cast<Layer*>(std::as_const(*this).get_layer_at_bottomz(bottom_z, epsilon)); }


} // namespace Slic3r
