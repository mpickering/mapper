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
#include <iterator>

#include <QtGlobal>
#include <QFileDevice>
#include <QIODevice>
#include <QLatin1String>
#include <QObject>
#include <QPointF>
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


namespace OpenOrienteering {

namespace {

/// \todo De-duplicate (ocd_file_import.cpp)
static QTextCodec* codecFromSettings()
{
	const auto& settings = Settings::getInstance();
	const auto name = settings.getSetting(Settings::General_Local8BitEncoding).toByteArray();
	return Util::codecForName(name);
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
void OcdFileExport::initEncoding()
{
	custom_8bit_encoding = nullptr;
}


template<>
void OcdFileExport::initEncoding<Ocd::Custom8BitEncoding>()
{
	custom_8bit_encoding = codecFromSettings();
	if (!custom_8bit_encoding)
	{
		addWarning(tr("Encoding '%1' is not available. Check the settings.")
		           .arg(Settings::getInstance().getSetting(Settings::General_Local8BitEncoding).toString()));
		custom_8bit_encoding = QTextCodec::codecForLocale();
	}
}



void OcdFileExport::doExport()
{
	auto actual_version = default_version;
	if (auto file = qobject_cast<QFileDevice*>(stream))
	{
		auto name = file->fileName().toUtf8();
		if (name.endsWith("test-v8.ocd"))
			actual_version = 8;
		else if (name.endsWith("test-v9.ocd"))
			actual_version = 9;
		else if (name.endsWith("test-v10.ocd"))
			actual_version = 10;
		else if (name.endsWith("test-v11.ocd"))
			actual_version = 11;
		else if (name.endsWith("test-v12.ocd"))
			actual_version = 12;
	}
	
	switch (actual_version)
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
		            arg(tr("OCD files of version %1 are not supported!").arg(actual_version))
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


template<class Format>
void OcdFileExport::exportImplementation(quint16 actual_version)
{
	addWarning(QLatin1String("OcdFileExport: WORK IN PROGRESS, FILE INCOMPLETE"));
	
	initEncoding<typename Format::Encoding>();
	
	OcdFile<Format> file;
	file.header()->version = actual_version;
	exportGeoreferencing(file);
	exportColors(file);
	exportSymbols(file);
	/*
	exportExtras(file);
	exportObjects(file);
	exportTemplates(file);
	exportView(file);
	*/
	stream->write(file.constByteArray());
}


template<>
void OcdFileExport::exportGeoreferencing(OcdFile<Ocd::FormatV8>& file)
{
	auto georef = map->getGeoreferencing();
	
	auto setup_pos = file.header()->setup_pos;
	Q_ASSERT(setup_pos);
	Q_ASSERT(int(setup_pos + sizeof(Ocd::SetupV8)) <= file.byteArray().size());
	
	auto setup = reinterpret_cast<Ocd::SetupV8*>(file.byteArray().data()+setup_pos);
	setup->map_scale = georef.getScaleDenominator();
	setup->real_offset_x = georef.getProjectedRefPoint().x();
	setup->real_offset_y = georef.getProjectedRefPoint().y();
	if (!qIsNull(georef.getGrivation()))
		setup->real_angle = georef.getGrivation();
}

namespace {
	QString makeString1039(const Map& map, quint16 version)
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
}

template<class File>
void OcdFileExport::exportGeoreferencing(File& file)
{
	using Format = typename File::Format;
	auto entity = QByteArray(toOcdString(makeString1039(*map, Format::version)));
	file.strings().insert(1039, entity);
}


template<>
void OcdFileExport::exportColors(OcdFile<Ocd::FormatV8>& file)
{
	Ocd::SymbolHeaderV8& symbol_header = file.header()->symbol_header;
	
	auto num_colors = map->getNumColors();
	if (num_colors > 256)
	{
		/// \todo Handle, or even throw error.
		addWarning(OcdFileExport::tr("Too many colors for OCD version 8."));
		num_colors = 256;
	}
	
	symbol_header.num_colors = decltype(symbol_header.num_colors)(num_colors);
	for (int i = 0; i < num_colors; i++)
	{
		const auto color = map->getColor(i);
		
		Ocd::ColorInfoV8& color_info = symbol_header.color_info[i];
		color_info.number = decltype(color_info.number)(i);
		color_info.name = toOcdString(color->getName());
		
		// OC*D stores CMYK values as integers from 0-200.
		const auto& cmyk = color->getCmyk();
		color_info.cmyk.cyan = decltype(color_info.cmyk.cyan)(qRound(200 * cmyk.c));
		color_info.cmyk.magenta = decltype(color_info.cmyk.magenta)(qRound(200 * cmyk.m));
		color_info.cmyk.yellow = decltype(color_info.cmyk.yellow)(qRound(200 * cmyk.y));
		color_info.cmyk.black = decltype(color_info.cmyk.black)(qRound(200 * cmyk.k));
		
		using std::begin;
		using std::end;
		std::fill(begin(color_info.separations), end(color_info.separations), 0);
	}
	
	addWarning(OcdFileExport::tr("Spot color information was ignored."));
}

namespace {
	QString makeString9(const MapColor& color)
	{
		const auto& cmyk = color.getCmyk();
		QString string_9;
		QTextStream out(&string_9, QIODevice::Append);
		out << color.getName()
		    << "\tn" << color.getPriority()
		    << "\tc" << qRound(cmyk.c * 100)
		    << "\tm" << qRound(cmyk.m * 100)
		    << "\ty" << qRound(cmyk.y * 100)
		    << "\tk" << qRound(cmyk.k * 100)
		    << "\to" << (color.getKnockout() ? '0' : '1')
		    << "\tt" << qRound(color.getOpacity() * 100);
		qDebug("String9 : %s", qPrintable(string_9));
		return string_9;
	}
}

template<class File>
void OcdFileExport::exportColors(File& file)
{
	auto num_colors = map->getNumColors();
	for (int i = 0; i < num_colors; i++)
	{
		const auto color = *map->getColor(i);
		file.strings().insert(9, toOcdString(makeString9(color)));
	}
	
	addWarning(OcdFileExport::tr("Spot color information was ignored."));
}


template< class OcdBaseSymbol >
void OcdFileExport::setupBaseSymbol(OcdBaseSymbol& ocd_base_symbol, const Symbol* symbol)
{
	ocd_base_symbol = {};
	ocd_base_symbol.description = toOcdString(symbol->getName());
	auto number = symbol->getNumberComponent(0) * OcdBaseSymbol::symbol_number_factor;
	if (symbol->getNumberComponent(1) >= 0)
		number += symbol->getNumberComponent(1) % OcdBaseSymbol::symbol_number_factor;
	// Symbol number 0.0 is not valid
	ocd_base_symbol.number = number ? decltype(ocd_base_symbol.number)(number) : 1;
#if 0
	// Ensure uniqueness of the symbol number
	while (symbol_numbers.find(ocad_symbol->number) != symbol_numbers.end())
		++ocad_symbol->number;
	symbol_numbers.insert(ocad_symbol->number);
#endif
	if (symbol->isProtected())
		ocd_base_symbol.status |= Ocd::SymbolProtected;
	if (symbol->isHidden())
		ocd_base_symbol.status |= Ocd::SymbolHidden;

#if 0
	// Set of used colors
	u8 bitmask = 1;
	u8* bitpos = ocad_symbol->colors;
	for (int c = 0; c < map->getNumColors(); ++c)
	{
		if (symbol->containsColor(map->getColor(c)))
			*bitpos |= bitmask;
		
		bitmask = bitmask << 1;
		if (bitmask == 0)
		{
			bitmask = 1;
			++bitpos;
		}
	}
	exportSymbolIcon(symbol, ocad_symbol->icon);
#endif
}



template< class OcdPointSymbol >
QByteArray OcdFileExport::exportPointSymbol(const PointSymbol* point_symbol)
{
	OcdPointSymbol ocd_symbol = {};
	setupBaseSymbol<typename OcdPointSymbol::BaseSymbol>(ocd_symbol.base, point_symbol);
	ocd_symbol.base.type = Ocd::SymbolTypePoint;
	if (point_symbol->isRotatable())
		ocd_symbol.base.flags |= 1;
	
	QByteArray data(reinterpret_cast<const char*>(&ocd_symbol), sizeof(OcdPointSymbol) - sizeof(typename OcdPointSymbol::Element));
	/// \todo Append elements
	return data;
}



template<class File>
void OcdFileExport::exportSymbols(File& file)
{
	using Format = typename File::Format;
	
	QByteArray ocd_symbol;
	
	auto num_colors = map->getNumColors();
	if (Format::version <= 8 && num_colors > 256)
		num_colors = 256;
	const auto num_symbols = map->getNumSymbols();
	for (int i = 0; i < num_symbols; ++i)
	{
		ocd_symbol.clear();
		
		const Symbol* symbol = map->getSymbol(i);
		switch(symbol->getType())
		{
		case Symbol::Point:
			ocd_symbol = exportPointSymbol<typename Format::PointSymbol>(static_cast<const PointSymbol*>(symbol));
			break;
			
		default:
			qInfo("Unhandled symbol type: %d", int(symbol->getType()));
			continue;
		}
		
		Q_ASSERT(!ocd_symbol.isEmpty());
		file.symbols().insert(ocd_symbol);
			
#if 0
		
		if (symbol->getType() == Symbol::Point)
			index = exportPointSymbol(symbol->asPoint());
		else if (symbol->getType() == Symbol::Line)
			index = exportLineSymbol(symbol->asLine());
		else if (symbol->getType() == Symbol::Area)
			index = exportAreaSymbol(symbol->asArea());
		else if (symbol->getType() == Symbol::Text)
			index = exportTextSymbol(symbol->asText());
		else if (symbol->getType() == Symbol::Combined)
			; // This is done as a second pass to ensure that all dependencies are added to the symbol_index
		else
			Q_ASSERT(false);
		
		if (index >= 0)
		{
			std::set<s16> number;
			number.insert(index);
			symbol_index.insert(symbol, number);
		}
#endif
	}
}


}  // namespace OpenOrienteering
