/*
 *    Copyright 2016-2018 Kai Pastor
 *
 *    Some parts taken from file_format_oc*d8{.h,_p.h,cpp} which are
 *    Copyright 2012 Pete Curtis
 *
 *    This file is part of OpenOrienteering.
 *
 *    OpenOrienteering is free software: you can redistribute it and/or modify
 *    it under the terms of the GNU General Public License as published by
 *    the Free Software Foundation, either version 3 of the License, or
 *    (at your option) any later version.
 *
 *    OpenOrienteering is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    GNU General Public License for more details.
 *
 *    You should have received a copy of the GNU General Public License
 *    along with OpenOrienteering.  If not, see <http://www.gnu.org/licenses/>.
 */

#include "ocd_file_export.h"

#include <algorithm>
#include <cstddef>
#include <cstdlib>
#include <iterator>
#include <memory>
#include <type_traits>
#include <vector>

#include <QtGlobal>
#include <QtMath>
#include <QColor>
#include <QCoreApplication>
#include <QFileDevice>
#include <QImage>
#include <QIODevice>
#include <QLatin1String>
#include <QObject>
#include <QPoint>
#include <QPointF>
#include <QRectF>
#include <QRgb>
#include <QString>
#include <QTextCodec>
#include <QTextStream>
#include <QVariant>

#include "settings.h"
#include "core/georeferencing.h"
#include "core/map.h"
#include "core/map_color.h"
#include "core/map_coord.h"
#include "core/map_grid.h"
#include "core/map_part.h"
#include "core/map_view.h"
#include "core/objects/object.h"
#include "core/symbols/area_symbol.h"
#include "core/symbols/line_symbol.h"
#include "core/symbols/point_symbol.h"
#include "core/symbols/symbol.h"
#include "fileformats/file_format.h"
#include "fileformats/file_import_export.h"
#include "fileformats/ocad8_file_format_p.h"
#include "fileformats/ocd_types.h"
#include "fileformats/ocd_types_v8.h"
#include "fileformats/ocd_types_v9.h"
#include "fileformats/ocd_types_v11.h"
#include "fileformats/ocd_types_v12.h"
#include "util/encoding.h"
#include "util/util.h"


namespace OpenOrienteering {

namespace {

/// \todo De-duplicate (ocd_file_import.cpp)
static QTextCodec* codecFromSettings()
{
	const auto& settings = Settings::getInstance();
	const auto name = settings.getSetting(Settings::General_Local8BitEncoding).toByteArray();
	return Util::codecForName(name);
}



constexpr qint32 convertPointMember(qint32 value)
{
	return (value < -5) ? qint32(0x80000000u | ((0x7fffffu & quint32((value-4)/10)) << 8)) : qint32((0x7fffffu & quint32((value+5)/10)) << 8);
}

// convertPointMember() shall round half up.
Q_STATIC_ASSERT(convertPointMember(-16) == qint32(0xfffffe00u)); // __ down __
Q_STATIC_ASSERT(convertPointMember(-15) == qint32(0xffffff00u)); //     up
Q_STATIC_ASSERT(convertPointMember( -6) == qint32(0xffffff00u)); // __ down __
Q_STATIC_ASSERT(convertPointMember( -5) == qint32(0x00000000u)); //     up
Q_STATIC_ASSERT(convertPointMember( -1) == qint32(0x00000000u)); //     up
Q_STATIC_ASSERT(convertPointMember(  0) == qint32(0x00000000u)); //  unchanged
Q_STATIC_ASSERT(convertPointMember( +1) == qint32(0x00000000u)); //    down
Q_STATIC_ASSERT(convertPointMember( +4) == qint32(0x00000000u)); // __ down __
Q_STATIC_ASSERT(convertPointMember( +5) == qint32(0x00000100u)); //     up
Q_STATIC_ASSERT(convertPointMember(+14) == qint32(0x00000100u)); // __ down __
Q_STATIC_ASSERT(convertPointMember(+15) == qint32(0x00000200u)); //     up


Ocd::OcdPoint32 convertPoint(qint32 x, qint32 y)
{
	return { convertPointMember(x), convertPointMember(-y) };
}


Ocd::OcdPoint32 convertPoint(const MapCoord& coord)
{
	return convertPoint(coord.nativeX(), coord.nativeY());
}


constexpr qint16 convertSize(qint32 size)
{
	return qint16((size+5) / 10);
}

constexpr qint32 convertSize(qint64 size)
{
	return qint32((size+5) / 10);
}


int convertRotation(float angle)
{
	return qRound(10 * qRadiansToDegrees(angle));
}


int getPaletteColorV6(QRgb rgb)
{
	Q_ASSERT(qAlpha(rgb) == 255);
	
	// Quickly return for most frequent value
	if (rgb == qRgb(255, 255, 255))
		return 15;
	
	auto color = QColor(rgb).toHsv();
	if (color.hue() == -1 || color.saturation() < 32)
	{
		auto gray = qGray(rgb);  // qGray is used for dithering
		if (gray >= 192)
			return 8;
		if (gray >= 128)
			return 7;
		return 0;
	}
	
	struct PaletteColor
	{
		int hue;
		int saturation;
		int value;
	};
	static const PaletteColor palette[16] = {
	    {  -1,   0,   0 },
	    {   0, 255, 128 },
	    { 120, 255, 128 },
	    {  60, 255, 128 },
	    { 240, 255, 128 },
	    { 300, 255, 128 },
	    { 180, 255, 128 },
	    {  -1,   0, 128 },
	    {  -1,   0, 192 },
	    {   0, 255, 255 },
	    { 120, 255, 255 },
	    {  60, 255, 255 },
	    { 240, 255, 255 },
	    { 300, 255, 255 },
	    { 180, 255, 255 },
	    {  -1,   0, 255 },
	};
	
#if 0
	static auto generate = true;
	if (generate)
	{
		static const QColor original_palette[16] = {
			QColor(  0,   0,   0).toHsv(),
			QColor(128,   0,   0).toHsv(),
			QColor(0,   128,   0).toHsv(),
			QColor(128, 128,   0).toHsv(),
			QColor(  0,   0, 128).toHsv(),
			QColor(128,   0, 128).toHsv(),
			QColor(  0, 128, 128).toHsv(),
			QColor(128, 128, 128).toHsv(),
			QColor(192, 192, 192).toHsv(),
			QColor(255,   0,   0).toHsv(),
			QColor(  0, 255,   0).toHsv(),
			QColor(255, 255,   0).toHsv(),
			QColor(  0,   0, 255).toHsv(),
			QColor(255,   0, 255).toHsv(),
			QColor(  0, 255, 255).toHsv(),
			QColor(255, 255, 255).toHsv()
		};
		
		for (auto& c : original_palette)
		{
			qDebug("		{ %3d, %3d, %3d },", c.hue(), c.saturation(), c.value());
		}
		generate = false;
	}
#endif
	
	int best_index = 0;
	auto best_distance = 2100000;  // > 6 * (10*sq(180) + sq(128) + sq(64))
	auto sq = [](int n) { return n*n; };
	for (auto i : { 1, 2, 3, 4, 5, 6, 9, 10, 11, 12, 13, 14 })
	{
		// True color
		const auto& palette_color = palette[i];
		auto hue_dist = std::abs(color.hue() - palette_color.hue);
		auto distance = 10 * sq(std::min(hue_dist, 360 - hue_dist))
		                + sq(color.saturation() - palette_color.saturation)
		                + sq(color.value() - palette_color.value);
		
		// (Too much) manual tweaking for orienteering colors
		if (i == 1)
			distance *= 3;	// Dark red
		else if (i == 3)
			distance *= 4;		// Olive
		else if (i == 11)
			distance *= 4;		// Yellow
		else if (i == 9)
			distance *= 6;		// Red is unlikely
		else
			distance *= 2;
		
		if (distance < best_distance)
		{
			best_distance = distance;
			best_index = i;
		}
	}
	return best_index;
}


quint8 getPaletteColorV9(QRgb rgb)
{
	Q_ASSERT(qAlpha(rgb) == 255);
	
	// Quickly return most frequent value
	if (rgb == qRgb(255, 255, 255))
		return 124;
	
	struct PaletteColor
	{
		int r;
		int g;
		int b;
	};
	static const PaletteColor palette[125] = {
	    {   0,   0,   0 },
	    {   0,   0,  64 },
	    {   0,   0, 128 },
	    {   0,   0, 192 },
	    {   0,   0, 255 },
	    {   0,  64,   0 },
	    {   0,  64,  64 },
	    {   0,  64, 128 },
	    {   0,  64, 192 },
	    {   0,  64, 255 },
	    {   0, 128,   0 },
	    {   0, 128,  64 },
	    {   0, 128, 128 },
	    {   0, 128, 192 },
	    {   0, 128, 255 },
	    {   0, 192,   0 },
	    {   0, 192,  64 },
	    {   0, 192, 128 },
	    {   0, 192, 192 },
	    {   0, 192, 255 },
	    {   0, 255,   0 },
	    {   0, 255,  64 },
	    {   0, 255, 128 },
	    {   0, 255, 192 },
	    {   0, 255, 255 },
	    {  64,   0,   0 },
	    {  64,   0,  64 },
	    {  64,   0, 128 },
	    {  64,   0, 192 },
	    {  64,   0, 255 },
	    {  64,  64,   0 },
	    {  64,  64,  64 },
	    {  64,  64, 128 },
	    {  64,  64, 192 },
	    {  64,  64, 255 },
	    {  64, 128,   0 },
	    {  64, 128,  64 },
	    {  64, 128, 128 },
	    {  64, 128, 192 },
	    {  64, 128, 255 },
	    {  64, 192,   0 },
	    {  64, 192,  64 },
	    {  64, 192, 128 },
	    {  64, 192, 192 },
	    {  64, 192, 255 },
	    {  64, 255,   0 },
	    {  64, 255,  64 },
	    {  64, 255, 128 },
	    {  64, 255, 192 },
	    {  64, 255, 255 },
	    { 128,   0,   0 },
	    { 128,   0,  64 },
	    { 128,   0, 128 },
	    { 128,   0, 192 },
	    { 128,   0, 255 },
	    { 128,  64,   0 },
	    { 128,  64,  64 },
	    { 128,  64, 128 },
	    { 128,  64, 192 },
	    { 128,  64, 255 },
	    { 128, 128,   0 },
	    { 128, 128,  64 },
	    { 128, 128, 128 },
	    { 128, 128, 192 },
	    { 128, 128, 255 },
	    { 128, 192,   0 },
	    { 128, 192,  64 },
	    { 128, 192, 128 },
	    { 128, 192, 192 },
	    { 128, 192, 255 },
	    { 128, 255,   0 },
	    { 128, 255,  64 },
	    { 128, 255, 128 },
	    { 128, 255, 192 },
	    { 128, 255, 255 },
	    { 192,   0,   0 },
	    { 192,   0,  64 },
	    { 192,   0, 128 },
	    { 192,   0, 192 },
	    { 192,   0, 255 },
	    { 192,  64,   0 },
	    { 192,  64,  64 },
	    { 192,  64, 128 },
	    { 192,  64, 192 },
	    { 192,  64, 255 },
	    { 192, 128,   0 },
	    { 192, 128,  64 },
	    { 192, 128, 128 },
	    { 192, 128, 192 },
	    { 192, 128, 255 },
	    { 192, 192,   0 },
	    { 192, 192,  64 },
	    { 192, 192, 128 },
	    { 192, 192, 192 },
	    { 192, 192, 255 },
	    { 192, 255,   0 },
	    { 192, 255,  64 },
	    { 192, 255, 128 },
	    { 192, 255, 192 },
	    { 192, 255, 255 },
	    { 255,   0,   0 },
	    { 255,   0,  64 },
	    { 255,   0, 128 },
	    { 255,   0, 192 },
	    { 255,   0, 255 },
	    { 255,  64,   0 },
	    { 255,  64,  64 },
	    { 255,  64, 128 },
	    { 255,  64, 192 },
	    { 255,  64, 255 },
	    { 255, 128,   0 },
	    { 255, 128,  64 },
	    { 255, 128, 128 },
	    { 255, 128, 192 },
	    { 255, 128, 255 },
	    { 255, 192,   0 },
	    { 255, 192,  64 },
	    { 255, 192, 128 },
	    { 255, 192, 192 },
	    { 255, 192, 255 },
	    { 255, 255,   0 },
	    { 255, 255,  64 },
	    { 255, 255, 128 },
	    { 255, 255, 192 },
	    { 255, 255, 255 },
	};
	
#if 0
	static auto generate = true;
	if (generate)
	{
		static const int color_levels[5] = { 0x00, 0x40, 0x80, 0xc0, 0xff };
		for (auto r : color_levels)
		{
			for (auto g : color_levels)
			{
				for (auto b : color_levels)
				{
					qDebug("		{ %3d, %3d, %3d },", r, g, b);
				}
			}
		}
		generate = false;
	}
#endif
	
	auto r = qRed(rgb);
	auto g = qGreen(rgb);
	auto b = qBlue(rgb);
	auto sq = [](int n) { return n*n; };

	quint8 best_index = 0;
	auto best_distance = 10000; // > (2 + 3 + 4) * sq(32)
	for (quint8 i = 0; i < 125; ++i)
	{
		auto palette_color = palette[i];
		auto distance = 2 * sq(r - palette_color.r) + 4 * sq(g - palette_color.g) + 3 * sq(b - palette_color.b);
		if (distance < best_distance)
		{
			best_distance = distance;
			best_index = i;
		}
	}
	return best_index;
}



int getPatternSize(const PointSymbol* point)
{
	if (!point)
		return 0;
	
	int count = 0;
	for (int i = 0; i < point->getNumElements(); ++i)
	{
		int factor = 1;
		if (point->getElementSymbol(i)->getType() == Symbol::Point)
		{
			factor = 0;
			const PointSymbol* point_symbol = static_cast<const PointSymbol*>(point->getElementSymbol(i));
			if (point_symbol->getInnerRadius() > 0 && point_symbol->getInnerColor())
				++factor;
			if (point_symbol->getOuterWidth() > 0 && point_symbol->getOuterColor())
				++factor;
		}
		count += factor * int(2 + point->getElementObject(i)->getRawCoordinateVector().size());
	}
	if (point->getInnerRadius() > 0 && point->getInnerColor())
		count += 2 + 1;
	if (point->getOuterWidth() > 0 && point->getOuterColor())
		count += 2 + 1;
	
	return count * int(sizeof(Ocd::OcdPoint32));
}



/// String 9: color
QString stringForColor(int i, const MapColor& color)
{
	const auto& cmyk = color.getCmyk();
	QString string_9;
	QTextStream out(&string_9, QIODevice::Append);
	out << color.getName()
	    << "\tn" << i
	    << "\tc" << qRound(cmyk.c * 100)
	    << "\tm" << qRound(cmyk.m * 100)
	    << "\ty" << qRound(cmyk.y * 100)
	    << "\tk" << qRound(cmyk.k * 100)
	    << "\to" << (color.getKnockout() ? '0' : '1')
	    << "\tt" << qRound(color.getOpacity() * 100);
	return string_9;
}


/// String 1039: georeferencing and grid
QString stringForScalePar(const Map& map, quint16 version)
{
	const auto& georef = map.getGeoreferencing();
	const auto ref_point = georef.toProjectedCoords(MapCoord{});
	
	auto& grid = map.getGrid();
	auto grid_spacing_real = 500.0;
	auto grid_spacing_map  = 50.0;
	auto spacing = std::min(grid.getHorizontalSpacing(), grid.getVerticalSpacing());
	switch (grid.getUnit())
	{
	case MapGrid::MillimetersOnMap:
		grid_spacing_map = spacing;
		grid_spacing_real = spacing * georef.getScaleDenominator()  / 1000;
		break;
	case MapGrid::MetersInTerrain:
		grid_spacing_map = spacing * 1000 / georef.getScaleDenominator();
		grid_spacing_real = spacing;
		break;
	}
	
	QString string_1039;
	QTextStream out(&string_1039, QIODevice::Append);
	out << fixed
	    << "\tm" << georef.getScaleDenominator()
	    << qSetRealNumberPrecision(4)
	    << "\tg" << grid_spacing_map
	    << "\tr" << /* real world coordinates */ 1
	    << "\tx" << qRound(ref_point.x())
	    << "\ty" << qRound(ref_point.y())
	    << qSetRealNumberPrecision(8)
	    << "\ta" << georef.getGrivation()
	    << qSetRealNumberPrecision(6)
	    << "\td" << grid_spacing_real
	    << "\ti" << /* combined_grid_zone */ 0;
	if (version > 9)
	{
		out << qSetRealNumberPrecision(2)
		    << "\tb" << 0.0
		    << "\tc" << 0.0;
	}
	return string_1039;	
}

} // namespace



int OcdFileExport::default_version = 0;



OcdFileExport::OcdFileExport(QIODevice* stream, Map* map, MapView* view)
: Exporter { stream, map, view }
{
	// nothing else
}


OcdFileExport::~OcdFileExport() = default;



template<class Encoding>
QTextCodec* OcdFileExport::determineEncoding()
{
	return nullptr;
}


template<>
QTextCodec* OcdFileExport::determineEncoding<Ocd::Custom8BitEncoding>()
{
	auto encoding = codecFromSettings();
	if (!encoding)
	{
		addWarning(tr("Encoding '%1' is not available. Check the settings.")
		           .arg(Settings::getInstance().getSetting(Settings::General_Local8BitEncoding).toString()));
		encoding = QTextCodec::codecForLocale();
	}
	return encoding;
}



void OcdFileExport::doExport()
{
	auto version = default_version;
	if (auto file = qobject_cast<QFileDevice*>(stream))
	{
		auto name = file->fileName().toUtf8();
		if (name.endsWith("test-v8.ocd"))
			version = 8;
		else if (name.endsWith("test-v9.ocd"))
			version = 9;
		else if (name.endsWith("test-v10.ocd"))
			version = 10;
		else if (name.endsWith("test-v11.ocd"))
			version = 11;
		else if (name.endsWith("test-v12.ocd"))
			version = 12;
	}
	
	switch (version)
	{
	case 0:
		exportImplementationLegacy();
		break;
		
	case 8:
		exportImplementation<Ocd::FormatV8>();
		break;
		
	case 9:
		exportImplementation<Ocd::FormatV9>();
		break;
		
	case 10:
		exportImplementation<Ocd::FormatV9>(10);
		break;
		
	case 11:
		exportImplementation<Ocd::FormatV11>();
		break;
		
	case 12:
		exportImplementation<Ocd::FormatV12>();
		break;
		
	default:
		throw FileFormatException(
		            Exporter::tr("Could not write file: %1").
		            arg(tr("OCD files of version %1 are not supported!").arg(version))
		            );
	}
}



void OcdFileExport::exportImplementationLegacy()
{
	OCAD8FileExport delegate { stream, map, view };
	delegate.doExport();
	for (auto&& w : delegate.warnings())
	{
		addWarning(w);
	}
}


namespace {

void setupFileHeaderGeneric(quint16 actual_version, Ocd::FileHeaderGeneric& header)
{
	header.version = actual_version;
	switch (actual_version)
	{
	case 8:
		header.file_type = Ocd::TypeMapV8;
		break;
	default:
		header.file_type = Ocd::TypeMap;
	}
}

} // namespace


template<class Format>
void OcdFileExport::exportImplementation(quint16 actual_version)
{
	addWarning(QLatin1String("OcdFileExport: WORK IN PROGRESS, FILE INCOMPLETE"));
	
	ocd_version = actual_version;
	
	OcdFile<Format> file;
	
	custom_8bit_encoding = determineEncoding<typename Format::Encoding>();
	if (custom_8bit_encoding)
	{
		addParameterString = [&file, this](qint32 string_type, const QString& string) {
			file.strings().insert(string_type, custom_8bit_encoding->fromUnicode(string));
		};
	}
	else
	{
		addParameterString = [&file](qint32 string_type, const QString& string) {
			file.strings().insert(string_type, string.toUtf8());
		};
	}
	
	// Check for a neccessary offset (and add related warnings early).
	area_offset = calculateAreaOffset();
	uses_registration_color = map->isColorUsedByASymbol(map->getRegistrationColor());
	
	setupFileHeaderGeneric(actual_version, *file.header());
	exportSetup(file);   // includes colors
	exportSymbols(file);
	exportObjects(file);
	exportExtras(file);
	
	stream->write(file.constByteArray());
}


MapCoord OcdFileExport::calculateAreaOffset()
{
	auto area_offset = QPointF{};
	
	// Attention: When changing ocd_bounds, update the warning messages, too.
	auto ocd_bounds = QRectF{QPointF{-2000, -2000}, QPointF{2000, 2000}};
	auto objects_extent = map->calculateExtent();
	if (objects_extent.isValid() && !ocd_bounds.contains(objects_extent))
	{
		if (objects_extent.width() < ocd_bounds.width()
		    && objects_extent.height() < ocd_bounds.height())
		{
			// The extent fits into the limited area.
			addWarning(tr("Coordinates are adjusted to fit into the OCAD 8 drawing area (-2 m ... 2 m)."));
			area_offset = objects_extent.center();
		}
		else
		{
			// The extent is too wide to fit.
			
			// Only move the objects if they are completely outside the drawing area.
			// This avoids repeated moves on open/save/close cycles.
			if (!objects_extent.intersects(ocd_bounds))
			{
				addWarning(tr("Coordinates are adjusted to fit into the OCAD 8 drawing area (-2 m ... 2 m)."));
				std::size_t count = 0;
				auto calculate_average_center = [&area_offset, &count](Object* object) {
					area_offset *= qreal(count)/qreal(count+1);
					++count;
					area_offset += object->getExtent().center() / count;
				};
				map->applyOnAllObjects(calculate_average_center);
			}
			
			addWarning(tr("Some coordinates remain outside of the OCAD 8 drawing area."
			              " They might be unreachable in OCAD."));
		}
		
		if (area_offset.manhattanLength() > 0)
		{
			// Round offset to 100 m in projected coordinates, to avoid crude grid offset.
			constexpr auto unit = 100;
			auto projected_offset = map->getGeoreferencing().toProjectedCoords(MapCoordF(area_offset));
			projected_offset.rx() = qreal(qRound(projected_offset.x()/unit)) * unit;
			projected_offset.ry() = qreal(qRound(projected_offset.y()/unit)) * unit;
			area_offset = map->getGeoreferencing().toMapCoordF(projected_offset);
		}
	}
	
	return MapCoord{area_offset};
}



template<>
void OcdFileExport::exportSetup(OcdFile<Ocd::FormatV8>& file)
{
	{
		auto setup = reinterpret_cast<Ocd::SetupV8*>(file.byteArray().data() + file.header()->setup_pos);
		
		auto georef = map->getGeoreferencing();
		setup->map_scale = georef.getScaleDenominator();
		setup->real_offset_x = georef.getProjectedRefPoint().x();
		setup->real_offset_y = georef.getProjectedRefPoint().y();
		if (!qIsNull(georef.getGrivation()))
			setup->real_angle = georef.getGrivation();
		
		if (view)
		{
			setup->center = convertPoint(view->center() - area_offset);
			setup->zoom = view->getZoom();
		}
		else
		{
			setup->zoom = 1;
		}
	}
	
	{	
		auto notes = custom_8bit_encoding->fromUnicode(map->getMapNotes());
		if (!notes.isEmpty())
		{
			auto size = notes.size() + 1;
			if (size > 32768)
			{
				/// \todo addWarning(...)
				size = 32768;
				notes.truncate(23767);
			}
			file.header()->info_pos = quint32(file.byteArray().size());
			file.header()->info_size = quint32(size);
			file.byteArray().append(notes).append('\0');
		}
	}
		
	{
		auto& symbol_header = file.header()->symbol_header;
		
		auto num_colors = map->getNumColors();
		if (num_colors > (uses_registration_color ? 255 : 256))
			throw FileFormatException(tr("The map contains more than 256 colors which is not supported by ocd version 8."));
		
		auto addColor = [&symbol_header, this](const MapColor* color, quint16 ocd_number) {
			auto& color_info = symbol_header.color_info[ocd_number];
			color_info.number = ocd_number;
			color_info.name = toOcdString(color->getName());
			
			// OC*D stores CMYK values as integers from 0-200.
			const auto& cmyk = color->getCmyk();
			color_info.cmyk.cyan    = quint8(qRound(200 * cmyk.c));
			color_info.cmyk.magenta = quint8(qRound(200 * cmyk.m));
			color_info.cmyk.yellow  = quint8(qRound(200 * cmyk.y));
			color_info.cmyk.black   = quint8(qRound(200 * cmyk.k));
			
			std::fill(std::begin(color_info.separations), std::end(color_info.separations), 0);
		};
		
		quint16 ocd_number = 0;
		if (uses_registration_color)
		{
			addWarning(tr("Registration black is exported as a regular color."));
			addColor(Map::getRegistrationColor(), ocd_number++);
		}
		for (int i = 0; i < num_colors; ++i)
		{
			addColor(map->getColor(i), ocd_number++);
		}
		symbol_header.num_colors = ocd_number;
		
		addWarning(OcdFileExport::tr("Spot color information was ignored."));
	}
}


template<class Format>
void OcdFileExport::exportSetup(OcdFile<Format>& file)
{
	exportSetup(file.header()->version);
}


void OcdFileExport::exportSetup(quint16 ocd_version)
{
	// Georeferencing
	addParameterString(1039, stringForScalePar(*map, ocd_version));
	
	// Map notes
	if (ocd_version >= 9)
		addParameterString(ocd_version >= 11 ? 1061 : 11, map->getMapNotes());
	
	// Map colors
	int ocd_number = 0;
	if (uses_registration_color)
	{
		addWarning(tr("Registration black is exported as a regular color."));
		addParameterString(9, stringForColor(ocd_number++, *Map::getRegistrationColor()));
	}
	auto num_colors = map->getNumColors();
	for (int i = 0; i < num_colors; ++i)
	{
		addParameterString(9, stringForColor(ocd_number++, *map->getColor(i)));
	}
	
	addWarning(OcdFileExport::tr("Spot color information was ignored."));
}



template<class Format>
void OcdFileExport::exportSymbols(OcdFile<Format>& file)
{
	qint32 index_number = 0;
	const auto num_symbols = map->getNumSymbols();
	for (int i = 0; i < num_symbols; ++i)
	{
		QByteArray ocd_symbol;
		
		const auto symbol = map->getSymbol(i);
		switch(symbol->getType())
		{
		case Symbol::Point:
			ocd_symbol = exportPointSymbol<typename Format::PointSymbol>(static_cast<const PointSymbol*>(symbol));
			break;
		
		case Symbol::Area:
			ocd_symbol = exportAreaSymbol<typename Format::AreaSymbol>(static_cast<const AreaSymbol*>(symbol));
			break;
			
		case Symbol::Line:
			ocd_symbol = exportLineSymbol<typename Format::LineSymbol>(static_cast<const LineSymbol*>(symbol));
			break;
			
		case Symbol::Text:
		case Symbol::Combined:
			qInfo("Unhandled symbol type: %d", int(symbol->getType()));
			continue;
			
		case Symbol::NoSymbol:
		case Symbol::AllSymbols:
			Q_UNREACHABLE();
		}
		
		Q_ASSERT(!ocd_symbol.isEmpty());
		file.symbols().insert(ocd_symbol);
		++index_number;
	}
}


template< class OcdBaseSymbol >
void OcdFileExport::setupBaseSymbol(const Symbol* symbol, OcdBaseSymbol& ocd_base_symbol)
{
	ocd_base_symbol = {};
	ocd_base_symbol.description = toOcdString(symbol->getName());
	auto number = symbol->getNumberComponent(0) * OcdBaseSymbol::symbol_number_factor;
	if (symbol->getNumberComponent(1) >= 0)
		number += symbol->getNumberComponent(1) % OcdBaseSymbol::symbol_number_factor;
	// Symbol number 0.0 is not valid
	ocd_base_symbol.number = number ? decltype(ocd_base_symbol.number)(number) : 1;
	// Ensure uniqueness of the symbol number
	auto matches_symbol_number = [&ocd_base_symbol](auto entry) { return ocd_base_symbol.number == entry.second; };
	while (std::any_of(begin(symbol_numbers), end(symbol_numbers), matches_symbol_number))
		++ocd_base_symbol.number;
	symbol_numbers[symbol] = ocd_base_symbol.number;
	
	if (symbol->isProtected())
		ocd_base_symbol.status |= Ocd::SymbolProtected;
	if (symbol->isHidden())
		ocd_base_symbol.status |= Ocd::SymbolHidden;

	// Set of used colors
	using ColorBitmask = typename std::remove_extent<typename std::remove_pointer<decltype(ocd_base_symbol.colors)>::type>::type;
	Q_STATIC_ASSERT(std::is_unsigned<ColorBitmask>::value);
	ColorBitmask bitmask = 1;
	
	auto bitpos = std::begin(ocd_base_symbol.colors);
	auto last = std::end(ocd_base_symbol.colors);
	if (uses_registration_color && symbol->containsColor(map->getRegistrationColor()))
	{
		*bitpos |= bitmask;
		bitmask *= 2;
	}
	for (int c = 0; c < map->getNumColors(); ++c)
	{
		if (symbol->containsColor(map->getColor(c)))
			*bitpos |= bitmask;
		
		bitmask *= 2;
		if (bitmask == 0) 
		{
			bitmask = 1;
			++bitpos;
			if (++bitpos == last)
				break;
		}
	}
	
	switch (std::extent<typename std::remove_pointer<decltype(ocd_base_symbol.icon_bits)>::type>::value)
	{
	case 264:
		exportSymbolIconV6(symbol, ocd_base_symbol.icon_bits);
		break;
		
	case 484:
		exportSymbolIconV9(symbol, ocd_base_symbol.icon_bits);
		break;
	}
}


template< class OcdPointSymbol >
QByteArray OcdFileExport::exportPointSymbol(const PointSymbol* point_symbol)
{
	OcdPointSymbol ocd_symbol = {};
	setupBaseSymbol<typename OcdPointSymbol::BaseSymbol>(point_symbol, ocd_symbol.base);
	ocd_symbol.base.type = Ocd::SymbolTypePoint;
	ocd_symbol.base.extent = decltype(ocd_symbol.base.extent)(getPointSymbolExtent(point_symbol));
	if (ocd_symbol.base.extent <= 0)
		ocd_symbol.base.extent = 100;
	if (point_symbol->isRotatable())
		ocd_symbol.base.flags |= 1;
	
	auto pattern_size = getPatternSize(point_symbol);
	auto header_size = int(sizeof(OcdPointSymbol) - sizeof(typename OcdPointSymbol::Element));
	ocd_symbol.base.size = decltype(ocd_symbol.base.size)(header_size + pattern_size);
	ocd_symbol.data_size = decltype(ocd_symbol.data_size)(pattern_size / 8);
	
	QByteArray data;
	data.reserve(header_size + pattern_size);
	data.append(reinterpret_cast<const char*>(&ocd_symbol), header_size);
	exportPattern<typename OcdPointSymbol::Element>(point_symbol, data);
	Q_ASSERT(data.size() == header_size + pattern_size);
	
	return data;
}


template< class Element >
qint16 OcdFileExport::exportPattern(const PointSymbol* point, QByteArray& byte_array)
{
	if (!point)
		return 0;
	
	auto num_coords = exportSubPattern<Element>({ {} }, point, byte_array);
	for (int i = 0; i < point->getNumElements(); ++i)
	{
		num_coords += exportSubPattern<Element>(point->getElementObject(i)->getRawCoordinateVector(), point->getElementSymbol(i), byte_array);
	}
	return num_coords;
}


template< class Element >
qint16 OcdFileExport::exportSubPattern(const MapCoordVector& coords, const Symbol* symbol, QByteArray& byte_array)
{
	auto makeElement = [](QByteArray& byte_array)->Element& {
		auto pos = byte_array.size();
		const auto proto_element = Element {};
		byte_array.append(reinterpret_cast<const char *>(&proto_element), sizeof(proto_element));
		return *reinterpret_cast<Element*>(byte_array.data() + pos);
	};
	
	qint16 num_coords = 0;
    
	switch (symbol->getType())
	{
	case Symbol::Point:
		{
			auto point_symbol = static_cast<const PointSymbol*>(symbol);
			if (point_symbol->getInnerRadius() > 0 && point_symbol->getInnerColor())
			{
				auto& element = makeElement(byte_array);
				element.type = Element::TypeDot;
				element.color = convertColor(point_symbol->getInnerColor());
				element.diameter = convertSize(2 * point_symbol->getInnerRadius());
				element.num_coords = exportCoordinates(coords, symbol, byte_array);
				num_coords += 2 + element.num_coords;
			}
			if (point_symbol->getOuterWidth() > 0 && point_symbol->getOuterColor())
			{
				auto& element = makeElement(byte_array);
				element.type = Element::TypeCircle;
				element.color = convertColor(point_symbol->getOuterColor());
				element.line_width = convertSize(point_symbol->getOuterWidth());
				if (ocd_version <= 8)
					element.diameter = convertSize(2 * point_symbol->getInnerRadius() + 2 * point_symbol->getOuterWidth());
				else
					element.diameter = convertSize(2 * point_symbol->getInnerRadius() + point_symbol->getOuterWidth());
				element.num_coords = exportCoordinates(coords, symbol, byte_array);
				num_coords += 2 + element.num_coords;
			}
		}
		break;
		
	case Symbol::Line:
		{
			const LineSymbol* line_symbol = static_cast<const LineSymbol*>(symbol);
			auto& element = makeElement(byte_array);
			element.type = Element::TypeLine;
			if (line_symbol->getCapStyle() == LineSymbol::RoundCap)
				element.flags |= 1;
			else if (line_symbol->getJoinStyle() == LineSymbol::MiterJoin)
				element.flags |= 4;
			element.color = convertColor(line_symbol->getColor());
			element.line_width = convertSize(line_symbol->getLineWidth());
			element.num_coords = exportCoordinates(coords, symbol, byte_array);
			num_coords += 2 + element.num_coords;
		}
		break;
		
	case Symbol::Area:
		{
			const AreaSymbol* area_symbol = static_cast<const AreaSymbol*>(symbol);
			auto& element = makeElement(byte_array);
			element.type = Element::TypeArea;
			element.color = convertColor(area_symbol->getColor());
			element.num_coords = exportCoordinates(coords, symbol, byte_array);
			num_coords += 2 + element.num_coords;
		}
		break;
		
	case Symbol::NoSymbol:
	case Symbol::AllSymbols:
	case Symbol::Combined:
	case Symbol::Text:
		Q_UNREACHABLE();
	}
	
	return num_coords;
}


template< class OcdAreaSymbol >
QByteArray OcdFileExport::exportAreaSymbol(const AreaSymbol* area_symbol)
{
	const PointSymbol* pattern_symbol = nullptr;
	
	OcdAreaSymbol ocd_symbol = {};
	setupBaseSymbol<typename OcdAreaSymbol::BaseSymbol>(area_symbol, ocd_symbol.base);
	ocd_symbol.base.type = Ocd::SymbolTypeArea;
	ocd_symbol.base.flags = exportAreaSymbolCommon(area_symbol, ocd_symbol.common, pattern_symbol);
	exportAreaSymbolSpecial<OcdAreaSymbol>(area_symbol, ocd_symbol);
	
	auto pattern_size = getPatternSize(pattern_symbol);
	auto header_size = int(sizeof(OcdAreaSymbol) - sizeof(typename OcdAreaSymbol::Element));
	ocd_symbol.base.size = decltype(ocd_symbol.base.size)(header_size + pattern_size);
	ocd_symbol.data_size = decltype(ocd_symbol.data_size)(pattern_size / 8);
	
	QByteArray data;
	data.reserve(header_size + pattern_size);
	data.append(reinterpret_cast<const char*>(&ocd_symbol), header_size);
	exportPattern<typename OcdAreaSymbol::Element>(pattern_symbol, data);
	Q_ASSERT(data.size() == header_size + pattern_size);
	
	return data;
}


template< class OcdAreaSymbolCommon >
quint8 OcdFileExport::exportAreaSymbolCommon(const AreaSymbol* area_symbol, OcdAreaSymbolCommon& ocd_area_common, const PointSymbol*& pattern_symbol)
{
	if (area_symbol->getColor())
	{
		ocd_area_common.fill_on_V9 = 1;
		ocd_area_common.fill_color = convertColor(area_symbol->getColor());
	}
	
	quint8 flags = 0;
	// Hatch
	// ocd_area_common.hatch_mode = 0;
	for (int i = 0, end = area_symbol->getNumFillPatterns(); i < end; ++i)
	{
		const auto& pattern = area_symbol->getFillPattern(i);
		switch (pattern.type)
		{
		case AreaSymbol::FillPattern::LinePattern:
			switch (ocd_area_common.hatch_mode)
			{
			case Ocd::HatchNone:
				ocd_area_common.hatch_mode = Ocd::HatchSingle;
				ocd_area_common.hatch_color = convertColor(pattern.line_color);
				ocd_area_common.hatch_line_width = decltype(ocd_area_common.hatch_line_width)(convertSize(pattern.line_width));
				if (ocd_version <= 8)
					ocd_area_common.hatch_dist = decltype(ocd_area_common.hatch_dist)(convertSize(pattern.line_spacing - pattern.line_width));
				else
					ocd_area_common.hatch_dist = decltype(ocd_area_common.hatch_dist)(convertSize(pattern.line_spacing));
				ocd_area_common.hatch_angle_1 = decltype(ocd_area_common.hatch_angle_1)(convertRotation(pattern.angle));
				if (pattern.rotatable())
					flags |= 1;
				break;
			case Ocd::HatchSingle:
				if (ocd_area_common.hatch_color == convertColor(pattern.line_color))
				{
					ocd_area_common.hatch_mode = Ocd::HatchCross;
					ocd_area_common.hatch_line_width = decltype(ocd_area_common.hatch_line_width)(ocd_area_common.hatch_line_width + convertSize(pattern.line_width)) / 2;
					ocd_area_common.hatch_dist = decltype(ocd_area_common.hatch_dist)(ocd_area_common.hatch_dist + convertSize(pattern.line_spacing - pattern.line_width)) / 2;
					ocd_area_common.hatch_angle_2 = decltype(ocd_area_common.hatch_angle_2)(convertRotation(pattern.angle));
					if (pattern.rotatable())
						flags |= 1;
					break;
				}
				// fall through
			default:
				addWarning(tr("In area symbol \"%1\", skipping a fill pattern.").arg(area_symbol->getPlainTextName()));
			}
			break;
			
		case AreaSymbol::FillPattern::PointPattern:
		    switch (ocd_area_common.hatch_mode)
			{
			case Ocd::StructureNone:
				ocd_area_common.structure_mode = Ocd::StructureAlignedRows;
				ocd_area_common.structure_width = decltype(ocd_area_common.structure_width)(convertSize(pattern.point_distance));
				ocd_area_common.structure_height = decltype(ocd_area_common.structure_height)(convertSize(pattern.line_spacing));
				ocd_area_common.structure_angle = decltype(ocd_area_common.structure_angle)(convertRotation(pattern.angle));
				pattern_symbol = pattern.point;
				if (pattern.rotatable())
					flags |= 1;
				break;
			case Ocd::StructureAlignedRows:
				ocd_area_common.structure_mode = Ocd::StructureShiftedRows;
				// NOTE: This is only a heuristic which works for the
				// orienteering symbol sets, not a general conversion.
				// (Conversion is not generally posssible.)
				// No further checks are done to find out if the conversion
				// is applicable because with these checks. Already a tiny
				// (not noticeable) error in the symbol definition would make
				// it take the wrong choice.
				addWarning(tr("In area symbol \"%1\", assuming a \"shifted rows\" point pattern. This might be correct as well as incorrect.")
				           .arg(area_symbol->getPlainTextName()));
				
				if (pattern.line_offset != 0)
					ocd_area_common.structure_height /= 2;
				else
					ocd_area_common.structure_width /= 2;
				
				break;
			default:
				addWarning(tr("In area symbol \"%1\", skipping a fill pattern.").arg(area_symbol->getPlainTextName()));
			}
		}
	}
	return flags;
}


template< >
void OcdFileExport::exportAreaSymbolSpecial<Ocd::AreaSymbolV8>(const AreaSymbol* /*area_symbol*/, Ocd::AreaSymbolV8& ocd_area_symbol)
{
	ocd_area_symbol.fill_on = ocd_area_symbol.common.fill_on_V9;
	ocd_area_symbol.common.fill_on_V9 = 0;
}


template< class OcdAreaSymbol >
void OcdFileExport::exportAreaSymbolSpecial(const AreaSymbol* /*area_symbol*/, OcdAreaSymbol& /*ocd_area_symbol*/)
{
	// nothing
}



template< class OcdLineSymbol >
QByteArray OcdFileExport::exportLineSymbol(const LineSymbol* line_symbol)
{
	OcdLineSymbol ocd_symbol = {};
	setupBaseSymbol<typename OcdLineSymbol::BaseSymbol>(line_symbol, ocd_symbol.base);
	ocd_symbol.base.type = Ocd::SymbolTypeLine;
	
	auto extent = quint16(convertSize(line_symbol->getLineWidth()/2));
	if (line_symbol->hasBorder())
	{
		const auto& border = line_symbol->getBorder();
		extent += convertSize(std::max(0, border.shift + border.width / 2));
	}
	extent = std::max(extent, getPointSymbolExtent(line_symbol->getStartSymbol()));
	extent = std::max(extent, getPointSymbolExtent(line_symbol->getEndSymbol()));
	extent = std::max(extent, getPointSymbolExtent(line_symbol->getMidSymbol()));
	extent = std::max(extent, getPointSymbolExtent(line_symbol->getDashSymbol()));
	ocd_symbol.base.extent = decltype(ocd_symbol.base.extent)(extent);
	
	auto pattern_size = exportLineSymbolCommon(line_symbol, ocd_symbol.common);
	auto header_size = sizeof(OcdLineSymbol) - sizeof(typename OcdLineSymbol::Element);
	ocd_symbol.base.size = decltype(ocd_symbol.base.size)(header_size + pattern_size);
	if (ocd_version >= 11)
	{
		if (ocd_symbol.common.secondary_data_size)
			ocd_symbol.common.active_symbols_V11 |= 0x08;
		if (ocd_symbol.common.corner_data_size)
			ocd_symbol.common.active_symbols_V11 |= 0x04;
		if (ocd_symbol.common.start_data_size)
			ocd_symbol.common.active_symbols_V11 |= 0x02;
		if (ocd_symbol.common.end_data_size)
			ocd_symbol.common.active_symbols_V11 |= 0x01;
	}
	QByteArray data;
	data.reserve(int(header_size + pattern_size));
	data.append(reinterpret_cast<const char*>(&ocd_symbol), int(header_size));
	exportPattern<typename OcdLineSymbol::Element>(line_symbol->getMidSymbol(), data);
	exportPattern<typename OcdLineSymbol::Element>(line_symbol->getDashSymbol(), data);
	exportPattern<typename OcdLineSymbol::Element>(line_symbol->getStartSymbol(), data);
	exportPattern<typename OcdLineSymbol::Element>(line_symbol->getEndSymbol(), data);
	Q_ASSERT(data.size() == int(header_size + pattern_size));
	
	return data;
}


template< class OcdLineSymbolCommon >
quint32 OcdFileExport::exportLineSymbolCommon(const LineSymbol* line_symbol, OcdLineSymbolCommon& ocd_line_common)
{
	if (line_symbol->getColor())
	{
		ocd_line_common.line_color = convertColor(line_symbol->getColor());
		ocd_line_common.line_width = convertSize(line_symbol->getLineWidth());
	}
	
	// Cap and Join
	if (line_symbol->getCapStyle() == LineSymbol::FlatCap && line_symbol->getJoinStyle() == LineSymbol::BevelJoin)
		ocd_line_common.line_style = 0;
	else if (line_symbol->getCapStyle() == LineSymbol::RoundCap && line_symbol->getJoinStyle() == LineSymbol::RoundJoin)
		ocd_line_common.line_style = 1;
	else if (line_symbol->getCapStyle() == LineSymbol::PointedCap && line_symbol->getJoinStyle() == LineSymbol::BevelJoin)
		ocd_line_common.line_style = 2;
	else if (line_symbol->getCapStyle() == LineSymbol::PointedCap && line_symbol->getJoinStyle() == LineSymbol::RoundJoin)
		ocd_line_common.line_style = 3;
	else if (line_symbol->getCapStyle() == LineSymbol::FlatCap && line_symbol->getJoinStyle() == LineSymbol::MiterJoin)
		ocd_line_common.line_style = 4;
	else if (line_symbol->getCapStyle() == LineSymbol::PointedCap && line_symbol->getJoinStyle() == LineSymbol::MiterJoin)
		ocd_line_common.line_style = 6;
	else
	{
		addWarning(tr("In line symbol \"%1\", cannot represent cap/join combination.").arg(line_symbol->getPlainTextName()));
		// Decide based on the caps
		if (line_symbol->getCapStyle() == LineSymbol::FlatCap)
			ocd_line_common.line_style = 0;
		else if (line_symbol->getCapStyle() == LineSymbol::RoundCap)
			ocd_line_common.line_style = 1;
		else if (line_symbol->getCapStyle() == LineSymbol::PointedCap)
			ocd_line_common.line_style = 3;
		else if (line_symbol->getCapStyle() == LineSymbol::SquareCap)
			ocd_line_common.line_style = 0;
	}
	
	if (line_symbol->getCapStyle() == LineSymbol::PointedCap)
	{
		ocd_line_common.dist_from_start = convertSize(line_symbol->getPointedCapLength());
		ocd_line_common.dist_from_end = convertSize(line_symbol->getPointedCapLength());
	}
	
	// Dash pattern
	if (line_symbol->isDashed())
	{
		if (line_symbol->getMidSymbol() && !line_symbol->getMidSymbol()->isEmpty())
		{
			if (line_symbol->getDashesInGroup() > 1)
				addWarning(tr("In line symbol \"%1\", neglecting the dash grouping.").arg(line_symbol->getPlainTextName()));
			
			ocd_line_common.main_length = convertSize(line_symbol->getDashLength() + line_symbol->getBreakLength());
			ocd_line_common.end_length = ocd_line_common.main_length / 2;
			ocd_line_common.main_gap = convertSize(line_symbol->getBreakLength());
		}
		else
		{
			if (line_symbol->getDashesInGroup() > 1)
			{
				if (line_symbol->getDashesInGroup() > 2)
					addWarning(tr("In line symbol \"%1\", the number of dashes in a group has been reduced to 2.").arg(line_symbol->getPlainTextName()));
				
				ocd_line_common.main_length = convertSize(2 * line_symbol->getDashLength() + line_symbol->getInGroupBreakLength());
				ocd_line_common.end_length = convertSize(2 * line_symbol->getDashLength() + line_symbol->getInGroupBreakLength());
				ocd_line_common.main_gap = convertSize(line_symbol->getBreakLength());
				ocd_line_common.sec_gap = convertSize(line_symbol->getInGroupBreakLength());
				ocd_line_common.end_gap = ocd_line_common.sec_gap;
			}
			else
			{
				ocd_line_common.main_length = convertSize(line_symbol->getDashLength());
				ocd_line_common.end_length = ocd_line_common.main_length / (line_symbol->getHalfOuterDashes() ? 2 : 1);
				ocd_line_common.main_gap = convertSize(line_symbol->getBreakLength());
			}
		}
	}
	else
	{
		ocd_line_common.main_length = convertSize(line_symbol->getSegmentLength());
		ocd_line_common.end_length = convertSize(line_symbol->getEndLength());
	}
	
	// Double line
	if (line_symbol->hasBorder() && (line_symbol->getBorder().isVisible() || line_symbol->getRightBorder().isVisible()))
	{
		ocd_line_common.double_width = convertSize(line_symbol->getLineWidth() - line_symbol->getBorder().width + 2 * line_symbol->getBorder().shift);
		if (line_symbol->getBorder().dashed && !line_symbol->getRightBorder().dashed)
			ocd_line_common.double_mode = 2;
		else
			ocd_line_common.double_mode = line_symbol->getBorder().dashed ? 3 : 1;
		// ocd_line_common.dflags
		
		ocd_line_common.double_left_width = convertSize(line_symbol->getBorder().width);
		ocd_line_common.double_right_width = convertSize(line_symbol->getRightBorder().width);
		
		ocd_line_common.double_left_color = convertColor(line_symbol->getBorder().color);
		ocd_line_common.double_right_color = convertColor(line_symbol->getRightBorder().color);
		
		if (line_symbol->getBorder().dashed)
		{
			ocd_line_common.double_length = convertSize(line_symbol->getBorder().dash_length);
			ocd_line_common.double_gap = convertSize(line_symbol->getBorder().break_length);
		}
		else if (line_symbol->getRightBorder().dashed)
		{
			ocd_line_common.double_length = convertSize(line_symbol->getRightBorder().dash_length);
			ocd_line_common.double_gap = convertSize(line_symbol->getRightBorder().break_length);
		}
		
		if (((line_symbol->getBorder().dashed && line_symbol->getRightBorder().dashed) &&
				(line_symbol->getBorder().dash_length != line_symbol->getRightBorder().dash_length ||
				line_symbol->getBorder().break_length != line_symbol->getRightBorder().break_length)) ||
			(!line_symbol->getBorder().dashed && line_symbol->getRightBorder().dashed))
		{
			addWarning(tr("In line symbol \"%1\", cannot export the borders correctly.").arg(line_symbol->getPlainTextName()));
		}
	}
	
	ocd_line_common.min_sym = line_symbol->getShowAtLeastOneSymbol() ? 0 : -1;
	ocd_line_common.num_prim_sym = decltype(ocd_line_common.num_prim_sym)(line_symbol->getMidSymbolsPerSpot());
	ocd_line_common.prim_sym_dist = convertSize(line_symbol->getMidSymbolDistance());
	
	ocd_line_common.primary_data_size = getPatternSize(line_symbol->getMidSymbol()) / 8;
	ocd_line_common.secondary_data_size = 0;
	ocd_line_common.corner_data_size = getPatternSize(line_symbol->getDashSymbol()) / 8;
	ocd_line_common.start_data_size = getPatternSize(line_symbol->getStartSymbol()) / 8;
	ocd_line_common.end_data_size = getPatternSize(line_symbol->getEndSymbol()) / 8;
	
	return 8 * (ocd_line_common.primary_data_size
	            + ocd_line_common.secondary_data_size
	            + ocd_line_common.corner_data_size
	            + ocd_line_common.start_data_size
	            + ocd_line_common.end_data_size);
}



void OcdFileExport::exportSymbolIconV6(const Symbol* symbol, quint8 icon_bits[])
{
	// Icon: 22x22 with 4 bit palette color, origin at bottom left
	constexpr int icon_size = 22;
	QImage image = symbol->createIcon(*map, icon_size, false)
	               .convertToFormat(QImage::Format_ARGB32_Premultiplied);
	
	auto process_pixel = [&image](int x, int y)->int {
		// Apply premultiplied pixel on white background
		auto premultiplied = image.pixel(x, y);
		auto alpha = qAlpha(premultiplied);
		auto r = 255 - alpha + qRed(premultiplied);
		auto g = 255 - alpha + qGreen(premultiplied);
		auto b = 255 - alpha + qBlue(premultiplied);
		auto pixel = qRgb(r, g, b);
		
		// Ordered dithering 2x2 threshold matrix, adjusted for o-map halftones
		static int threshold[4] = { 24, 192, 136, 80 };
		auto palette_color = getPaletteColorV6(pixel);
		switch (palette_color)
		{
		case 0:
			// Black to gray (50%)
			return  qGray(pixel) < 128-threshold[(x%2 + 2*(y%2))]/2 ? 0 : 7;
			
		case 7:
			// Gray (50%) to light gray 
			return  qGray(pixel) < 192-threshold[(x%2 + 2*(y%2))]/4 ? 7 : 8;
			
		case 8:
			// Light gray to white
			return  qGray(pixel) < 256-threshold[(x%2 + 2*(y%2))]/4 ? 8 : 15;
			
		case 15:
			// Pure white
			return palette_color;
			
		default:
			// Color to white
			return  QColor(pixel).saturation() >= threshold[(x%2 + 2*(y%2))] ? palette_color : 15;
		}
	};
	
	for (int y = icon_size - 1; y >= 0; --y)
	{
		for (int x = 0; x < icon_size; x += 2)
		{
			auto first = process_pixel(x, y);
			auto second = process_pixel(x+1, y);
			*(icon_bits++) = quint8((first << 4) + second);
		}
		icon_bits++;
	}
}

void OcdFileExport::exportSymbolIconV9(const Symbol* symbol, quint8 icon_bits[])
{
	// Icon: 22x22 with 8 bit palette color code, origin at bottom left
	constexpr int icon_size = 22;
	QImage image = symbol->createIcon(*map, icon_size, true)
	               .convertToFormat(QImage::Format_ARGB32_Premultiplied);
	
	auto process_pixel = [&image](int x, int y)->quint8 {
		// Apply premultiplied pixel on white background
		auto premultiplied = image.pixel(x, y);
		auto alpha = qAlpha(premultiplied);
		auto r = 255 - alpha + qRed(premultiplied);
		auto g = 255 - alpha + qGreen(premultiplied);
		auto b = 255 - alpha + qBlue(premultiplied);
		return getPaletteColorV9(qRgb(r, g, b));
	};
	
	for (int y = icon_size - 1; y >= 0; --y)
	{
		for (int x = 0; x < icon_size; ++x)
		{
			*(icon_bits++) = process_pixel(x, y);
		}
	}
}



template<class Format>
void OcdFileExport::exportObjects(OcdFile<Format>& file)
{
	for (int l = 0; l < map->getNumParts(); ++l)
	{
		auto part = map->getPart(std::size_t(l));
		for (int o = 0; o < part->getNumObjects(); ++o)
		{
			const auto* object = part->getObject(o);
			
			std::unique_ptr<Object> duplicate;
			if (area_offset.nativeX() != 0 || area_offset.nativeY() != 0)
			{
				// Create a safely managed duplicate and move it as needed.
				duplicate.reset(object->duplicate());
				duplicate->move(-area_offset);
				object = duplicate.get();
			}
			object->update();
			
			QByteArray ocd_object;
			auto entry = typename Format::Object::IndexEntryType {};
			
			switch (object->getType())
			{
			case Object::Point:
				ocd_object = exportPointObject<typename Format::Object>(static_cast<const PointObject*>(object), entry);
				break;
				
			case Object::Path:
				ocd_object = exportPathObject<typename Format::Object>(static_cast<const PathObject*>(object), entry);
				break;
				
			case Object::Text:
				qInfo("Unhandled object type: %d", int(object->getType()));
				continue;
			}
			
			Q_ASSERT(!ocd_object.isEmpty());
			file.objects().insert(ocd_object, entry);
		}
	}
}


template< class OcdObject >
void handleObjectExtras(const Object* /*object*/, OcdObject& ocd_object, typename OcdObject::IndexEntryType& entry)
{
	// Extra entry members since V9
	entry.type = ocd_object.type;
	entry.status = Ocd::ObjectNormal;
}


template< >
void handleObjectExtras<Ocd::ObjectV8>(const Object* /*object*/, typename Ocd::ObjectV8& /*ocd_object*/, typename Ocd::ObjectV8::IndexEntryType& /*entry*/)
{
	// nothing
}


template< class OcdObject >
QByteArray OcdFileExport::exportPointObject(const PointObject* point, typename OcdObject::IndexEntryType& entry)
{
	OcdObject ocd_object = {};
	ocd_object.type = 1;
	ocd_object.symbol = entry.symbol = decltype(entry.symbol)(symbol_numbers[point->getSymbol()]);
	ocd_object.angle = decltype(ocd_object.angle)(convertRotation(point->getRotation()));
	return exportObjectCommon(point, ocd_object, entry);
}


template< class OcdObject >
QByteArray OcdFileExport::exportPathObject(const PathObject* path, typename OcdObject::IndexEntryType& entry)
{
	OcdObject ocd_object = {};
	auto symbol = path->getSymbol();
	if (symbol && symbol->getType() == Symbol::Area)
		ocd_object.type = 3;
	else
		ocd_object.type = 2;
	ocd_object.symbol = entry.symbol = decltype(entry.symbol)(symbol_numbers[path->getSymbol()]);
	return exportObjectCommon(path, ocd_object, entry);
}


template< class OcdObject >
QByteArray OcdFileExport::exportObjectCommon(const Object* object, OcdObject& ocd_object, typename OcdObject::IndexEntryType& entry)
{
	auto& coords = object->getRawCoordinateVector();
	ocd_object.num_items = decltype(ocd_object.num_items)(coords.size());
	
	auto header_size = int(sizeof(OcdObject) - sizeof(Ocd::OcdPoint32));
	auto items_size = int(coords.size() * sizeof(Ocd::OcdPoint32));
	
	QByteArray data;
	data.reserve(header_size + items_size);
	data.append(reinterpret_cast<const char*>(&ocd_object), header_size);
	exportCoordinates(coords, object->getSymbol(), data);
	Q_ASSERT(data.size() == header_size + items_size);
	
	entry.bottom_left_bound = convertPoint(MapCoord(object->getExtent().bottomLeft()));
	entry.top_right_bound = convertPoint(MapCoord(object->getExtent().topRight()));
	entry.size = decltype(entry.size)((Ocd::addPadding(data).size()));
	if (ocd_version < 11)
		entry.size = (entry.size - decltype(entry.size)(header_size)) / sizeof(Ocd::OcdPoint32);
	
	handleObjectExtras(object, ocd_object, entry);

	return data;
}



template<class Format>
void OcdFileExport::exportExtras(OcdFile<Format>& /*file*/)
{
	exportExtras(ocd_version);
}


void OcdFileExport::exportExtras(quint16 ocd_version)
{
	Q_UNUSED(ocd_version);
}


quint16 OcdFileExport::convertColor(const MapColor* color) const
{
	auto index = map->findColorIndex(color);
	if (index >= 0)
	{
		return quint16(uses_registration_color ? (index + 1) : index);
	}
	return 0;
}


quint16 OcdFileExport::getPointSymbolExtent(const PointSymbol* symbol) const
{
	if (!symbol)
		return 0;
	
	QRectF extent;
	for (int i = 0; i < symbol->getNumElements(); ++i)
	{
		std::unique_ptr<Object> object { symbol->getElementObject(i)->duplicate() };
		object->setSymbol(symbol->getElementSymbol(i), true);
		object->update();
		rectIncludeSafe(extent, object->getExtent());
		object->clearRenderables();
	}
	auto extent_f = 0.5 * std::max(extent.width(), extent.height());
	if (symbol->getInnerColor())
		extent_f = std::max(extent_f, 0.001 * symbol->getInnerRadius());
	if (symbol->getOuterColor())
		extent_f = std::max(extent_f, 0.001 * (symbol->getInnerRadius() + symbol->getOuterWidth()));
	return quint16(convertSize(qRound(std::max(0.0, 1000 * extent_f))));
}


quint16 OcdFileExport::exportCoordinates(const MapCoordVector& coords, const Symbol* symbol, QByteArray& byte_array)
{
	quint16 num_points = 0;
	bool curve_start = false;
	bool hole_point = false;
	bool curve_continue = false;
	for (const auto& point : coords)
	{
		auto p = convertPoint(point);
		if (point.isDashPoint())
		{
			if (!symbol || symbol->getType() != Symbol::Line)
				p.y |= Ocd::OcdPoint32::FlagCorner;
			else
			{
				const LineSymbol* line_symbol = static_cast<const LineSymbol*>(symbol);
				if ((line_symbol->getDashSymbol() == nullptr || line_symbol->getDashSymbol()->isEmpty()) && line_symbol->isDashed())
					p.y |= Ocd::OcdPoint32::FlagDash;
				else
					p.y |= Ocd::OcdPoint32::FlagCorner;
			}
		}
		if (curve_start)
			p.x |= Ocd::OcdPoint32::FlagCtl1;
		if (hole_point)
			p.y |= Ocd::OcdPoint32::FlagHole;
		if (curve_continue)
			p.x |= Ocd::OcdPoint32::FlagCtl2;
		
		curve_continue = curve_start;
		curve_start = point.isCurveStart();
		hole_point = point.isHolePoint();
		
		byte_array.append(reinterpret_cast<const char*>(&p), sizeof(p));
		++num_points;
	}
	return num_points;
}


}  // namespace OpenOrienteering
