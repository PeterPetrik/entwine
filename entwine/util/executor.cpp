/******************************************************************************
* Copyright (c) 2016, Connor Manning (connor@hobu.co)
*
* Entwine -- Point cloud indexing
*
* Entwine is available under the terms of the LGPL2 license. See COPYING
* for specific license text and more information.
*
******************************************************************************/

#include <entwine/util/executor.hpp>

#include <pdal/BufferReader.hpp>
#include <pdal/Filter.hpp>
#include <pdal/GlobalEnvironment.hpp>
#include <pdal/QuickInfo.hpp>
#include <pdal/Reader.hpp>
#include <pdal/StageFactory.hpp>

#include <entwine/types/bbox.hpp>
#include <entwine/types/pooled-point-table.hpp>
#include <entwine/types/reprojection.hpp>
#include <entwine/types/schema.hpp>

namespace entwine
{

namespace
{
    Reprojection srsFoundOrDefault(
            const pdal::SpatialReference& found,
            const Reprojection& given)
    {
        if (given.hammer() || found.empty()) return given;
        else return Reprojection(found.getWKT(), given.out());
    }

    const auto env(([]()
    {
        pdal::GlobalEnvironment::startup();
        return true;
    })());
}

Executor::Executor(bool is3d)
    : m_is3d(is3d)
    , m_stageFactory(new pdal::StageFactory())
    , m_factoryMutex()
{ }

Executor::~Executor()
{ }

bool Executor::run(
        PooledPointTable& table,
        const std::string path,
        const Reprojection* reprojection)
{
    auto lock(getLock());
    const std::string driver(m_stageFactory->inferReaderDriver(path));

    if (driver.empty()) return false;

    if (pdal::Reader* reader = createReader(driver, path))
    {
        reader->prepare(table);
        pdal::Stage* executor(reader);

        if (reprojection)
        {
            const auto srs(
                    srsFoundOrDefault(
                        reader->getSpatialReference(), *reprojection));

            if (pdal::Filter* filter = createReprojectionFilter(srs))
            {
                filter->setInput(*reader);
                executor = filter;
            }
            else
            {
                return false;
            }
        }

        executor->prepare(table);
        lock.unlock();

        executor->execute(table);
        return true;
    }
    else
    {
        return false;
    }
}

bool Executor::good(const std::string path) const
{
    auto lock(getLock());
    return !m_stageFactory->inferReaderDriver(path).empty();
}

std::unique_ptr<Preview> Executor::preview(
        const std::string path,
        const Reprojection* reprojection)
{
    std::unique_ptr<Preview> result;

    auto lock(getLock());
    const std::string driver(m_stageFactory->inferReaderDriver(path));

    if (pdal::Reader* reader = createReader(driver, path))
    {
        const pdal::QuickInfo quick(reader->preview());

        if (!quick.valid()) return result;

        std::string srs;

        BBox bbox(
                Point(
                    quick.m_bounds.minx,
                    quick.m_bounds.miny,
                    quick.m_bounds.minz),
                Point(
                    quick.m_bounds.maxx,
                    quick.m_bounds.maxy,
                    quick.m_bounds.maxz),
                m_is3d);

        if (reprojection)
        {
            namespace DimId = pdal::Dimension::Id;

            pdal::PointTable table;
            auto layout(table.layout());
            layout->registerDim(DimId::X);
            layout->registerDim(DimId::Y);
            layout->registerDim(DimId::Z);
            layout->finalize();

            pdal::PointViewPtr view(new pdal::PointView(table));

            view->setField(DimId::X, 0, bbox.min().x);
            view->setField(DimId::Y, 0, bbox.min().y);
            view->setField(DimId::Z, 0, bbox.min().z);

            view->setField(DimId::X, 1, bbox.max().x);
            view->setField(DimId::Y, 1, bbox.max().y);
            view->setField(DimId::Z, 1, bbox.max().z);

            view->setField(DimId::X, 2, bbox.min().x);
            view->setField(DimId::Y, 2, bbox.max().y);
            view->setField(DimId::Z, 2, bbox.min().z);

            view->setField(DimId::X, 3, bbox.max().x);
            view->setField(DimId::Y, 3, bbox.min().y);
            view->setField(DimId::Z, 3, bbox.max().z);

            pdal::BufferReader buffer;
            buffer.addView(view);

            if (pdal::Filter* filter =
                    createReprojectionFilter(
                        srsFoundOrDefault(quick.m_srs, *reprojection)))
            {
                filter->setInput(buffer);

                filter->prepare(table);
                filter->execute(table);

                const double hi(std::numeric_limits<double>::max());
                const double lo(std::numeric_limits<double>::lowest());

                Point min(hi, hi, hi);
                Point max(lo, lo, lo);

                for (std::size_t i(0); i < 4; ++i)
                {
                    const Point p(
                            view->getFieldAs<double>(DimId::X, i),
                            view->getFieldAs<double>(DimId::Y, i),
                            view->getFieldAs<double>(DimId::Z, i));

                    min.x = std::min(min.x, p.x);
                    min.y = std::min(min.y, p.y);
                    min.z = std::min(min.z, p.z);

                    max.x = std::max(max.x, p.x);
                    max.y = std::max(max.y, p.y);
                    max.z = std::max(max.z, p.z);
                }

                bbox = BBox(min, max, m_is3d);

                srs = pdal::SpatialReference(reprojection->out()).getWKT();
            }
            else
            {
                return result;
            }
        }
        else
        {
            srs = quick.m_srs.getWKT();
        }

        result.reset(
                new Preview(
                    bbox,
                    quick.m_pointCount,
                    srs,
                    quick.m_dimNames));
    }

    return result;
}

pdal::Reader* Executor::createReader(
        const std::string driver,
        const std::string path) const
{
    if (!driver.empty())
    {
        if (pdal::Reader* reader = static_cast<pdal::Reader*>(
                m_stageFactory->createStage(driver)))
        {
            pdal::Options options;
            options.add(pdal::Option("filename", path));
            reader->setOptions(options);

            return reader;
        }
    }

    return nullptr;
}

pdal::Filter* Executor::createReprojectionFilter(
        const Reprojection& reproj) const
{
    if (reproj.in().empty())
    {
        throw std::runtime_error("No default SRS supplied, and none inferred");
    }

    if (pdal::Filter* filter =
            static_cast<pdal::Filter*>(
                m_stageFactory->createStage("filters.reprojection")))
    {
        pdal::Options options;
        options.add(pdal::Option("in_srs", reproj.in()));
        options.add(pdal::Option("out_srs", reproj.out()));
        filter->setOptions(options);
        return filter;
    }

    return nullptr;
}

std::unique_lock<std::mutex> Executor::getLock() const
{
    return std::unique_lock<std::mutex>(m_factoryMutex);
}

} // namespace entwine

