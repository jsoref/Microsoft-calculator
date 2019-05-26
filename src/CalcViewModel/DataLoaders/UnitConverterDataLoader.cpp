// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT License.

#include "pch.h"
#include "Common/AppResourceProvider.h"
#include "UnitConverterDataLoader.h"
#include "UnitConverterDataConstants.h"
#include "CurrencyDataLoader.h"

using namespace CalculatorApp::Common;
using namespace CalculatorApp::DataLoaders;
using namespace CalculatorApp::ViewModel;
using namespace Platform;
using namespace std;
using namespace Windows::ApplicationModel::Resources;
using namespace Windows::ApplicationModel::Resources::Core;
using namespace Windows::Globalization;

static constexpr bool CONVERT_WITH_OFFSET_FIRST = true;

UnitConverterDataLoader::UnitConverterDataLoader(GeographicRegion ^ region)
    : m_currentRegionCode(region->CodeTwoLetter)
{
    m_categoryList = make_shared<vector<UCM::Category>>();
    m_categoryToUnits = make_shared<UCM::CategoryToUnitVectorMap>();
    m_ratioMap = make_shared<UCM::UnitToUnitToConversionDataMap>();
}

vector<UCM::Category> UnitConverterDataLoader::LoadOrderedCategories()
{
    return *m_categoryList;
}

vector<UCM::Unit> UnitConverterDataLoader::LoadOrderedUnits(const UCM::Category& category)
{
    return m_categoryToUnits->at(category);
}

unordered_map<UCM::Unit, UCM::ConversionData, UCM::UnitHash> UnitConverterDataLoader::LoadOrderedRatios(const UCM::Unit& unit)
{
    return m_ratioMap->at(unit);
}

bool UnitConverterDataLoader::SupportsCategory(const UCM::Category& target)
{
    shared_ptr<vector<UCM::Category>> supportedCategories = nullptr;
    if (!m_categoryList->empty())
    {
        supportedCategories = m_categoryList;
    }
    else
    {
        GetCategories(supportedCategories);
    }

    static int currencyId = NavCategory::Serialize(ViewMode::Currency);
    auto itr = find_if(supportedCategories->begin(), supportedCategories->end(), [&](const UCM::Category& category) {
        return currencyId != category.id && target.id == category.id;
    });

    return itr != supportedCategories->end();
}

void UnitConverterDataLoader::LoadData()
{
    unordered_map<int, OrderedUnit> idToUnit;

    unordered_map<ViewMode, vector<OrderedUnit>> orderedUnitMap{};
    unordered_map<ViewMode, unordered_map<int, double>> categoryToUnitConversionDataMap{};
    unordered_map<int, unordered_map<int, UCM::ConversionData>> explicitConversionData{};

    // Load categories, units and conversion data into data structures. This will be then used to populate hashmaps used by CalcEngine and UI layer
    GetCategories(m_categoryList);
    GetUnits(orderedUnitMap);
    GetConversionData(categoryToUnitConversionDataMap);
    GetExplicitConversionData(explicitConversionData); // This is needed for temperature conversions

    m_categoryToUnits->clear();
    m_ratioMap->clear();
    for (UCM::Category objectCategory : *m_categoryList)
    {
        ViewMode categoryViewMode = NavCategory::Deserialize(objectCategory.id);
        assert(NavCategory::IsConverterViewMode(categoryViewMode));
        if (categoryViewMode == ViewMode::Currency)
        {
            // Currency is an ordered category but we do not want to process it here
            // because this function is not thread-safe and currency data is asynchronously
            // loaded.
            m_categoryToUnits->insert(pair<UCM::Category, std::vector<UCM::Unit>>(objectCategory, {}));
            continue;
        }

        vector<OrderedUnit> orderedUnits = orderedUnitMap[categoryViewMode];
        vector<UCM::Unit> unitList;

        // Sort the units by order
        sort(orderedUnits.begin(), orderedUnits.end(), [](const OrderedUnit& first, const OrderedUnit& second) { return first.order < second.order; });

        for (OrderedUnit u : orderedUnits)
        {
            unitList.push_back(static_cast<UCM::Unit>(u));
            idToUnit.insert(pair<int, OrderedUnit>(u.id, u));
        }

        // Save units per category
        m_categoryToUnits->insert(pair<UCM::Category, std::vector<UCM::Unit>>(objectCategory, unitList));

        // For each unit, populate the conversion data
        for (UCM::Unit unit : unitList)
        {
            unordered_map<UCM::Unit, UCM::ConversionData, UCM::UnitHash> conversions;

            if (explicitConversionData.find(unit.id) == explicitConversionData.end())
            {
                // Get the associated units for a category id
                unordered_map<int, double> unitConversions = categoryToUnitConversionDataMap.at(categoryViewMode);
                double unitFactor = unitConversions[unit.id];

                for (const auto& [id, conversionFactor] : unitConversions)
                {
                    if (idToUnit.find(id) == idToUnit.end())
                    {
                        // Optional units will not be in idToUnit but can be in unitConversions.
                        // For optional units that did not make it to the current set of units, just continue.
                        continue;
                    }

                    UCM::ConversionData parsedData = { 1.0, 0.0, false };
                    assert(conversionFactor > 0); // divide by zero assert
                    parsedData.ratio = unitFactor / conversionFactor;
                    conversions.insert(pair<UCM::Unit, UCM::ConversionData>(idToUnit.at(id), parsedData));
                }
            }
            else
            {
                unordered_map<int, UCM::ConversionData> unitConversions = explicitConversionData.at(unit.id);
                for (auto itr = unitConversions.begin(); itr != unitConversions.end(); ++itr)
                {
                    conversions.insert(pair<UCM::Unit, UCM::ConversionData>(idToUnit.at(itr->first), itr->second));
                }
            }

            m_ratioMap->insert(pair<UCM::Unit, unordered_map<UCM::Unit, UCM::ConversionData, UCM::UnitHash>>(unit, conversions));
        }
    }
}

void UnitConverterDataLoader::GetCategories(_In_ shared_ptr<vector<UCM::Category>> categoriesList)
{
    categoriesList->clear();
    auto converterCategory = NavCategoryGroup::CreateConverterCategory();
    for (auto const& category : converterCategory->Categories)
    {
        /* Id, CategoryName, SupportsNegative */
        categoriesList->emplace_back(NavCategory::Serialize(category->Mode), category->Name->Data(), category->SupportsNegative);
    }
}

void UnitConverterDataLoader::GetUnits(_In_ unordered_map<ViewMode, vector<OrderedUnit>>& unitMap)
{
    // US + Federated States of Micronesia, Marshall Islands, Palau
    bool useUSCustomaryAndFahrenheit =
        m_currentRegionCode == L"US" || m_currentRegionCode == L"FM" || m_currentRegionCode == L"MH" || m_currentRegionCode == L"PW";

    // useUSCustomaryAndFahrenheit + Liberia
    // Source: https://en.wikipedia.org/wiki/Metrication
    bool useUSCustomary = useUSCustomaryAndFahrenheit || m_currentRegionCode == L"LR";

    // Use 'Système International' (International System of Units - Metrics)
    bool useSI = !useUSCustomary;

    // useUSCustomaryAndFahrenheit + the Bahamas, the Cayman Islands and Liberia
    // Source: http://en.wikipedia.org/wiki/Fahrenheit
    bool useFahrenheit = useUSCustomaryAndFahrenheit || m_currentRegionCode == "BS" || m_currentRegionCode == "KY" || m_currentRegionCode == "LR";

    bool useWattInsteadOfKilowatt = m_currentRegionCode == "GB";

    // Use Pyeong, a Korean floorspace unit.
    // https://en.wikipedia.org/wiki/Korean_units_of_measurement#Area
    bool usePyeong = m_currentRegionCode == L"KP" || m_currentRegionCode == L"KR";

    vector<OrderedUnit> areaUnits;
    areaUnits.push_back(
        OrderedUnit{ UnitConverterUnits::Area_Acre, GetLocalizedStringName(L"UnitName_Acre"), GetLocalizedStringName(L"UnitAbbreviation_Acre"), 9 });
    areaUnits.push_back(
        OrderedUnit{ UnitConverterUnits::Area_Hectare, GetLocalizedStringName(L"UnitName_Hectare"), GetLocalizedStringName(L"UnitAbbreviation_Hectare"), 4 });
    areaUnits.push_back(OrderedUnit{ UnitConverterUnits::Area_SquareCentimeter,
                                     GetLocalizedStringName(L"UnitName_SquareCentimeter"),
                                     GetLocalizedStringName(L"UnitAbbreviation_SquareCentimeter"),
                                     2 });
    areaUnits.push_back(OrderedUnit{ UnitConverterUnits::Area_SquareFoot,
                                     GetLocalizedStringName(L"UnitName_SquareFoot"),
                                     GetLocalizedStringName(L"UnitAbbreviation_SquareFoot"),
                                     7,
                                     useSI,
                                     useUSCustomary,
                                     false });
    areaUnits.push_back(OrderedUnit{
        UnitConverterUnits::Area_SquareInch, GetLocalizedStringName(L"UnitName_SquareInch"), GetLocalizedStringName(L"UnitAbbreviation_SquareInch"), 6 });
    areaUnits.push_back(OrderedUnit{ UnitConverterUnits::Area_SquareKilometer,
                                     GetLocalizedStringName(L"UnitName_SquareKilometer"),
                                     GetLocalizedStringName(L"UnitAbbreviation_SquareKilometer"),
                                     5 });
    areaUnits.push_back(OrderedUnit{ UnitConverterUnits::Area_SquareMeter,
                                     GetLocalizedStringName(L"UnitName_SquareMeter"),
                                     GetLocalizedStringName(L"UnitAbbreviation_SquareMeter"),
                                     3,
                                     useUSCustomary,
                                     useSI,
                                     false });
    areaUnits.push_back(OrderedUnit{
        UnitConverterUnits::Area_SquareMile, GetLocalizedStringName(L"UnitName_SquareMile"), GetLocalizedStringName(L"UnitAbbreviation_SquareMile"), 10 });
    areaUnits.push_back(OrderedUnit{ UnitConverterUnits::Area_SquareMillimeter,
                                     GetLocalizedStringName(L"UnitName_SquareMillimeter"),
                                     GetLocalizedStringName(L"UnitAbbreviation_SquareMillimeter"),
                                     1 });
    areaUnits.push_back(OrderedUnit{
        UnitConverterUnits::Area_SquareYard, GetLocalizedStringName(L"UnitName_SquareYard"), GetLocalizedStringName(L"UnitAbbreviation_SquareYard"), 8 });
    areaUnits.push_back(OrderedUnit{
        UnitConverterUnits::Area_Hand, GetLocalizedStringName(L"UnitName_Hand"), GetLocalizedStringName(L"UnitAbbreviation_Hand"), 11, false, false, true });
    areaUnits.push_back(OrderedUnit{
        UnitConverterUnits::Area_Paper, GetLocalizedStringName(L"UnitName_Paper"), GetLocalizedStringName(L"UnitAbbreviation_Paper"), 12, false, false, true });
    areaUnits.push_back(OrderedUnit{ UnitConverterUnits::Area_SoccerField,
                                     GetLocalizedStringName(L"UnitName_SoccerField"),
                                     GetLocalizedStringName(L"UnitAbbreviation_SoccerField"),
                                     13,
                                     false,
                                     false,
                                     true });
    areaUnits.push_back(OrderedUnit{ UnitConverterUnits::Area_Castle,
                                     GetLocalizedStringName(L"UnitName_Castle"),
                                     GetLocalizedStringName(L"UnitAbbreviation_Castle"),
                                     14,
                                     false,
                                     false,
                                     true });
    if (usePyeong)
    {
        areaUnits.push_back(OrderedUnit{ UnitConverterUnits::Area_Pyeong,
                                         GetLocalizedStringName(L"UnitName_Pyeong"),
                                         GetLocalizedStringName(L"UnitAbbreviation_Pyeong"),
                                         15,
                                         false,
                                         false,
                                         false });
    }
    unitMap.emplace(ViewMode::Area, areaUnits);

    vector<OrderedUnit> dataUnits;
    dataUnits.push_back(
        OrderedUnit{ UnitConverterUnits::Data_Bit, GetLocalizedStringName(L"UnitName_Bit"), GetLocalizedStringName(L"UnitAbbreviation_Bit"), 1 });
    dataUnits.push_back(
        OrderedUnit{ UnitConverterUnits::Data_Byte, GetLocalizedStringName(L"UnitName_Byte"), GetLocalizedStringName(L"UnitAbbreviation_Byte"), 2 });
    dataUnits.push_back(
        OrderedUnit{ UnitConverterUnits::Data_Exabits, GetLocalizedStringName(L"UnitName_Exabits"), GetLocalizedStringName(L"UnitAbbreviation_Exabits"), 23 });
    dataUnits.push_back(OrderedUnit{
        UnitConverterUnits::Data_Exabytes, GetLocalizedStringName(L"UnitName_Exabytes"), GetLocalizedStringName(L"UnitAbbreviation_Exabytes"), 25 });
    dataUnits.push_back(OrderedUnit{
        UnitConverterUnits::Data_Exbibits, GetLocalizedStringName(L"UnitName_Exbibits"), GetLocalizedStringName(L"UnitAbbreviation_Exbibits"), 24 });
    dataUnits.push_back(OrderedUnit{
        UnitConverterUnits::Data_Exbibytes, GetLocalizedStringName(L"UnitName_Exbibytes"), GetLocalizedStringName(L"UnitAbbreviation_Exbibytes"), 26 });
    dataUnits.push_back(OrderedUnit{
        UnitConverterUnits::Data_Gibibits, GetLocalizedStringName(L"UnitName_Gibibits"), GetLocalizedStringName(L"UnitAbbreviation_Gibibits"), 12 });
    dataUnits.push_back(OrderedUnit{
        UnitConverterUnits::Data_Gibibytes, GetLocalizedStringName(L"UnitName_Gibibytes"), GetLocalizedStringName(L"UnitAbbreviation_Gibibytes"), 14 });
    dataUnits.push_back(
        OrderedUnit{ UnitConverterUnits::Data_Gigabit, GetLocalizedStringName(L"UnitName_Gigabit"), GetLocalizedStringName(L"UnitAbbreviation_Gigabit"), 11 });
    dataUnits.push_back(OrderedUnit{ UnitConverterUnits::Data_Gigabyte,
                                     GetLocalizedStringName(L"UnitName_Gigabyte"),
                                     GetLocalizedStringName(L"UnitAbbreviation_Gigabyte"),
                                     13,
                                     true,
                                     false,
                                     false });
    dataUnits.push_back(OrderedUnit{
        UnitConverterUnits::Data_Kibibits, GetLocalizedStringName(L"UnitName_Kibibits"), GetLocalizedStringName(L"UnitAbbreviation_Kibibits"), 4 });
    dataUnits.push_back(OrderedUnit{
        UnitConverterUnits::Data_Kibibytes, GetLocalizedStringName(L"UnitName_Kibibytes"), GetLocalizedStringName(L"UnitAbbreviation_Kibibytes"), 6 });
    dataUnits.push_back(
        OrderedUnit{ UnitConverterUnits::Data_Kilobit, GetLocalizedStringName(L"UnitName_Kilobit"), GetLocalizedStringName(L"UnitAbbreviation_Kilobit"), 3 });
    dataUnits.push_back(OrderedUnit{
        UnitConverterUnits::Data_Kilobyte, GetLocalizedStringName(L"UnitName_Kilobyte"), GetLocalizedStringName(L"UnitAbbreviation_Kilobyte"), 5 });
    dataUnits.push_back(OrderedUnit{
        UnitConverterUnits::Data_Mebibits, GetLocalizedStringName(L"UnitName_Mebibits"), GetLocalizedStringName(L"UnitAbbreviation_Mebibits"), 8 });
    dataUnits.push_back(OrderedUnit{
        UnitConverterUnits::Data_Mebibytes, GetLocalizedStringName(L"UnitName_Mebibytes"), GetLocalizedStringName(L"UnitAbbreviation_Mebibytes"), 10 });
    dataUnits.push_back(
        OrderedUnit{ UnitConverterUnits::Data_Megabit, GetLocalizedStringName(L"UnitName_Megabit"), GetLocalizedStringName(L"UnitAbbreviation_Megabit"), 7 });
    dataUnits.push_back(OrderedUnit{ UnitConverterUnits::Data_Megabyte,
                                     GetLocalizedStringName(L"UnitName_Megabyte"),
                                     GetLocalizedStringName(L"UnitAbbreviation_Megabyte"),
                                     9,
                                     false,
                                     true,
                                     false });
    dataUnits.push_back(OrderedUnit{
        UnitConverterUnits::Data_Pebibits, GetLocalizedStringName(L"UnitName_Pebibits"), GetLocalizedStringName(L"UnitAbbreviation_Pebibits"), 20 });
    dataUnits.push_back(OrderedUnit{
        UnitConverterUnits::Data_Pebibytes, GetLocalizedStringName(L"UnitName_Pebibytes"), GetLocalizedStringName(L"UnitAbbreviation_Pebibytes"), 22 });
    dataUnits.push_back(
        OrderedUnit{ UnitConverterUnits::Data_Petabit, GetLocalizedStringName(L"UnitName_Petabit"), GetLocalizedStringName(L"UnitAbbreviation_Petabit"), 19 });
    dataUnits.push_back(OrderedUnit{
        UnitConverterUnits::Data_Petabyte, GetLocalizedStringName(L"UnitName_Petabyte"), GetLocalizedStringName(L"UnitAbbreviation_Petabyte"), 21 });
    dataUnits.push_back(OrderedUnit{
        UnitConverterUnits::Data_Tebibits, GetLocalizedStringName(L"UnitName_Tebibits"), GetLocalizedStringName(L"UnitAbbreviation_Tebibits"), 16 });
    dataUnits.push_back(OrderedUnit{
        UnitConverterUnits::Data_Tebibytes, GetLocalizedStringName(L"UnitName_Tebibytes"), GetLocalizedStringName(L"UnitAbbreviation_Tebibytes"), 18 });
    dataUnits.push_back(
        OrderedUnit{ UnitConverterUnits::Data_Terabit, GetLocalizedStringName(L"UnitName_Terabit"), GetLocalizedStringName(L"UnitAbbreviation_Terabit"), 15 });
    dataUnits.push_back(OrderedUnit{
        UnitConverterUnits::Data_Terabyte, GetLocalizedStringName(L"UnitName_Terabyte"), GetLocalizedStringName(L"UnitAbbreviation_Terabyte"), 17 });
    dataUnits.push_back(OrderedUnit{
        UnitConverterUnits::Data_Yobibits, GetLocalizedStringName(L"UnitName_Yobibits"), GetLocalizedStringName(L"UnitAbbreviation_Yobibits"), 32 });
    dataUnits.push_back(OrderedUnit{
        UnitConverterUnits::Data_Yobibytes, GetLocalizedStringName(L"UnitName_Yobibytes"), GetLocalizedStringName(L"UnitAbbreviation_Yobibytes"), 34 });
    dataUnits.push_back(OrderedUnit{
        UnitConverterUnits::Data_Yottabit, GetLocalizedStringName(L"UnitName_Yottabit"), GetLocalizedStringName(L"UnitAbbreviation_Yottabit"), 31 });
    dataUnits.push_back(OrderedUnit{
        UnitConverterUnits::Data_Yottabyte, GetLocalizedStringName(L"UnitName_Yottabyte"), GetLocalizedStringName(L"UnitAbbreviation_Yottabyte"), 33 });
    dataUnits.push_back(OrderedUnit{
        UnitConverterUnits::Data_Zebibits, GetLocalizedStringName(L"UnitName_Zebibits"), GetLocalizedStringName(L"UnitAbbreviation_Zebibits"), 28 });
    dataUnits.push_back(OrderedUnit{
        UnitConverterUnits::Data_Zebibytes, GetLocalizedStringName(L"UnitName_Zebibytes"), GetLocalizedStringName(L"UnitAbbreviation_Zebibytes"), 30 });
    dataUnits.push_back(OrderedUnit{
        UnitConverterUnits::Data_Zetabits, GetLocalizedStringName(L"UnitName_Zetabits"), GetLocalizedStringName(L"UnitAbbreviation_Zetabits"), 27 });
    dataUnits.push_back(OrderedUnit{
        UnitConverterUnits::Data_Zetabytes, GetLocalizedStringName(L"UnitName_Zetabytes"), GetLocalizedStringName(L"UnitAbbreviation_Zetabytes"), 29 });
    dataUnits.push_back(OrderedUnit{ UnitConverterUnits::Data_FloppyDisk,
                                     GetLocalizedStringName(L"UnitName_FloppyDisk"),
                                     GetLocalizedStringName(L"UnitAbbreviation_FloppyDisk"),
                                     13,
                                     false,
                                     false,
                                     true });
    dataUnits.push_back(OrderedUnit{
        UnitConverterUnits::Data_CD, GetLocalizedStringName(L"UnitName_CD"), GetLocalizedStringName(L"UnitAbbreviation_CD"), 14, false, false, true });
    dataUnits.push_back(OrderedUnit{
        UnitConverterUnits::Data_DVD, GetLocalizedStringName(L"UnitName_DVD"), GetLocalizedStringName(L"UnitAbbreviation_DVD"), 15, false, false, true });
    unitMap.emplace(ViewMode::Data, dataUnits);

    vector<OrderedUnit> energyUnits;
    energyUnits.push_back(OrderedUnit{ UnitConverterUnits::Energy_BritishThermalUnit,
                                       GetLocalizedStringName(L"UnitName_BritishThermalUnit"),
                                       GetLocalizedStringName(L"UnitAbbreviation_BritishThermalUnit"),
                                       7 });
    energyUnits.push_back(
        OrderedUnit{ UnitConverterUnits::Energy_Calorie, GetLocalizedStringName(L"UnitName_Calorie"), GetLocalizedStringName(L"UnitAbbreviation_Calorie"), 4 });
    energyUnits.push_back(OrderedUnit{ UnitConverterUnits::Energy_ElectronVolt,
                                       GetLocalizedStringName(L"UnitName_Electron-Volt"),
                                       GetLocalizedStringName(L"UnitAbbreviation_Electron-Volt"),
                                       1 });
    energyUnits.push_back(OrderedUnit{
        UnitConverterUnits::Energy_FootPound, GetLocalizedStringName(L"UnitName_Foot-Pound"), GetLocalizedStringName(L"UnitAbbreviation_Foot-Pound"), 6 });
    energyUnits.push_back(OrderedUnit{ UnitConverterUnits::Energy_Joule,
                                       GetLocalizedStringName(L"UnitName_Joule"),
                                       GetLocalizedStringName(L"UnitAbbreviation_Joule"),
                                       2,
                                       true,
                                       false,
                                       false });
    energyUnits.push_back(OrderedUnit{ UnitConverterUnits::Energy_Kilocalorie,
                                       GetLocalizedStringName(L"UnitName_Kilocalorie"),
                                       GetLocalizedStringName(L"UnitAbbreviation_Kilocalorie"),
                                       5,
                                       false,
                                       true,
                                       false });
    energyUnits.push_back(OrderedUnit{
        UnitConverterUnits::Energy_Kilojoule, GetLocalizedStringName(L"UnitName_Kilojoule"), GetLocalizedStringName(L"UnitAbbreviation_Kilojoule"), 3 });
    energyUnits.push_back(OrderedUnit{ UnitConverterUnits::Energy_Battery,
                                       GetLocalizedStringName(L"UnitName_Battery"),
                                       GetLocalizedStringName(L"UnitAbbreviation_Battery"),
                                       8,
                                       false,
                                       false,
                                       true });
    energyUnits.push_back(OrderedUnit{ UnitConverterUnits::Energy_Banana,
                                       GetLocalizedStringName(L"UnitName_Banana"),
                                       GetLocalizedStringName(L"UnitAbbreviation_Banana"),
                                       9,
                                       false,
                                       false,
                                       true });
    energyUnits.push_back(OrderedUnit{ UnitConverterUnits::Energy_SliceOfCake,
                                       GetLocalizedStringName(L"UnitName_SliceOfCake"),
                                       GetLocalizedStringName(L"UnitAbbreviation_SliceOfCake"),
                                       10,
                                       false,
                                       false,
                                       true });
    unitMap.emplace(ViewMode::Energy, energyUnits);

    vector<OrderedUnit> lengthUnits;
    lengthUnits.push_back(OrderedUnit{ UnitConverterUnits::Length_Centimeter,
                                       GetLocalizedStringName(L"UnitName_Centimeter"),
                                       GetLocalizedStringName(L"UnitAbbreviation_Centimeter"),
                                       4,
                                       useUSCustomary,
                                       useSI,
                                       false });
    lengthUnits.push_back(
        OrderedUnit{ UnitConverterUnits::Length_Foot, GetLocalizedStringName(L"UnitName_Foot"), GetLocalizedStringName(L"UnitAbbreviation_Foot"), 8 });
    lengthUnits.push_back(OrderedUnit{ UnitConverterUnits::Length_Inch,
                                       GetLocalizedStringName(L"UnitName_Inch"),
                                       GetLocalizedStringName(L"UnitAbbreviation_Inch"),
                                       7,
                                       useSI,
                                       useUSCustomary,
                                       false });
    lengthUnits.push_back(OrderedUnit{
        UnitConverterUnits::Length_Kilometer, GetLocalizedStringName(L"UnitName_Kilometer"), GetLocalizedStringName(L"UnitAbbreviation_Kilometer"), 6 });
    lengthUnits.push_back(
        OrderedUnit{ UnitConverterUnits::Length_Meter, GetLocalizedStringName(L"UnitName_Meter"), GetLocalizedStringName(L"UnitAbbreviation_Meter"), 5 });
    lengthUnits.push_back(
        OrderedUnit{ UnitConverterUnits::Length_Micron, GetLocalizedStringName(L"UnitName_Micron"), GetLocalizedStringName(L"UnitAbbreviation_Micron"), 2 });
    lengthUnits.push_back(
        OrderedUnit{ UnitConverterUnits::Length_Mile, GetLocalizedStringName(L"UnitName_Mile"), GetLocalizedStringName(L"UnitAbbreviation_Mile"), 10 });
    lengthUnits.push_back(OrderedUnit{
        UnitConverterUnits::Length_Millimeter, GetLocalizedStringName(L"UnitName_Millimeter"), GetLocalizedStringName(L"UnitAbbreviation_Millimeter"), 3 });
    lengthUnits.push_back(OrderedUnit{
        UnitConverterUnits::Length_Nanometer, GetLocalizedStringName(L"UnitName_Nanometer"), GetLocalizedStringName(L"UnitAbbreviation_Nanometer"), 1 });
    lengthUnits.push_back(OrderedUnit{ UnitConverterUnits::Length_NauticalMile,
                                       GetLocalizedStringName(L"UnitName_NauticalMile"),
                                       GetLocalizedStringName(L"UnitAbbreviation_NauticalMile"),
                                       11 });
    lengthUnits.push_back(
        OrderedUnit{ UnitConverterUnits::Length_Yard, GetLocalizedStringName(L"UnitName_Yard"), GetLocalizedStringName(L"UnitAbbreviation_Yard"), 9 });
    lengthUnits.push_back(OrderedUnit{ UnitConverterUnits::Length_Paperclip,
                                       GetLocalizedStringName(L"UnitName_Paperclip"),
                                       GetLocalizedStringName(L"UnitAbbreviation_Paperclip"),
                                       12,
                                       false,
                                       false,
                                       true });
    lengthUnits.push_back(OrderedUnit{
        UnitConverterUnits::Length_Hand, GetLocalizedStringName(L"UnitName_Hand"), GetLocalizedStringName(L"UnitAbbreviation_Hand"), 13, false, false, true });
    lengthUnits.push_back(OrderedUnit{ UnitConverterUnits::Length_JumboJet,
                                       GetLocalizedStringName(L"UnitName_JumboJet"),
                                       GetLocalizedStringName(L"UnitAbbreviation_JumboJet"),
                                       14,
                                       false,
                                       false,
                                       true });
    unitMap.emplace(ViewMode::Length, lengthUnits);

    vector<OrderedUnit> powerUnits;
    powerUnits.push_back(OrderedUnit{ UnitConverterUnits::Power_BritishThermalUnitPerMinute,
                                      GetLocalizedStringName(L"UnitName_BTUPerMinute"),
                                      GetLocalizedStringName(L"UnitAbbreviation_BTUPerMinute"),
                                      5 });
    powerUnits.push_back(OrderedUnit{ UnitConverterUnits::Power_FootPoundPerMinute,
                                      GetLocalizedStringName(L"UnitName_Foot-PoundPerMinute"),
                                      GetLocalizedStringName(L"UnitAbbreviation_Foot-PoundPerMinute"),
                                      4 });
    powerUnits.push_back(OrderedUnit{ UnitConverterUnits::Power_Horsepower,
                                      GetLocalizedStringName(L"UnitName_Horsepower"),
                                      GetLocalizedStringName(L"UnitAbbreviation_Horsepower"),
                                      3,
                                      false,
                                      true,
                                      false });
    powerUnits.push_back(OrderedUnit{ UnitConverterUnits::Power_Kilowatt,
                                      GetLocalizedStringName(L"UnitName_Kilowatt"),
                                      GetLocalizedStringName(L"UnitAbbreviation_Kilowatt"),
                                      2,
                                      !useWattInsteadOfKilowatt });
    powerUnits.push_back(OrderedUnit{ UnitConverterUnits::Power_Watt,
                                      GetLocalizedStringName(L"UnitName_Watt"),
                                      GetLocalizedStringName(L"UnitAbbreviation_Watt"),
                                      1,
                                      useWattInsteadOfKilowatt });
    powerUnits.push_back(OrderedUnit{ UnitConverterUnits::Power_LightBulb,
                                      GetLocalizedStringName(L"UnitName_LightBulb"),
                                      GetLocalizedStringName(L"UnitAbbreviation_LightBulb"),
                                      6,
                                      false,
                                      false,
                                      true });
    powerUnits.push_back(OrderedUnit{
        UnitConverterUnits::Power_Horse, GetLocalizedStringName(L"UnitName_Horse"), GetLocalizedStringName(L"UnitAbbreviation_Horse"), 7, false, false, true });
    powerUnits.push_back(OrderedUnit{ UnitConverterUnits::Power_TrainEngine,
                                      GetLocalizedStringName(L"UnitName_TrainEngine"),
                                      GetLocalizedStringName(L"UnitAbbreviation_TrainEngine"),
                                      8,
                                      false,
                                      false,
                                      true });
    unitMap.emplace(ViewMode::Power, powerUnits);

    vector<OrderedUnit> tempUnits;
    tempUnits.push_back(OrderedUnit{ UnitConverterUnits::Temperature_DegreesCelsius,
                                     GetLocalizedStringName(L"UnitName_DegreesCelsius"),
                                     GetLocalizedStringName(L"UnitAbbreviation_DegreesCelsius"),
                                     1,
                                     useFahrenheit,
                                     !useFahrenheit,
                                     false });
    tempUnits.push_back(OrderedUnit{ UnitConverterUnits::Temperature_DegreesFahrenheit,
                                     GetLocalizedStringName(L"UnitName_DegreesFahrenheit"),
                                     GetLocalizedStringName(L"UnitAbbreviation_DegreesFahrenheit"),
                                     2,
                                     !useFahrenheit,
                                     useFahrenheit,
                                     false });
    tempUnits.push_back(OrderedUnit{
        UnitConverterUnits::Temperature_Kelvin, GetLocalizedStringName(L"UnitName_Kelvin"), GetLocalizedStringName(L"UnitAbbreviation_Kelvin"), 3 });
    unitMap.emplace(ViewMode::Temperature, tempUnits);

    vector<OrderedUnit> timeUnits;
    timeUnits.push_back(
        OrderedUnit{ UnitConverterUnits::Time_Day, GetLocalizedStringName(L"UnitName_Day"), GetLocalizedStringName(L"UnitAbbreviation_Day"), 6 });
    timeUnits.push_back(OrderedUnit{
        UnitConverterUnits::Time_Hour, GetLocalizedStringName(L"UnitName_Hour"), GetLocalizedStringName(L"UnitAbbreviation_Hour"), 5, true, false, false });
    timeUnits.push_back(OrderedUnit{
        UnitConverterUnits::Time_Microsecond, GetLocalizedStringName(L"UnitName_Microsecond"), GetLocalizedStringName(L"UnitAbbreviation_Microsecond"), 1 });
    timeUnits.push_back(OrderedUnit{
        UnitConverterUnits::Time_Millisecond, GetLocalizedStringName(L"UnitName_Millisecond"), GetLocalizedStringName(L"UnitAbbreviation_Millisecond"), 2 });
    timeUnits.push_back(OrderedUnit{ UnitConverterUnits::Time_Minute,
                                     GetLocalizedStringName(L"UnitName_Minute"),
                                     GetLocalizedStringName(L"UnitAbbreviation_Minute"),
                                     4,
                                     false,
                                     true,
                                     false });
    timeUnits.push_back(
        OrderedUnit{ UnitConverterUnits::Time_Second, GetLocalizedStringName(L"UnitName_Second"), GetLocalizedStringName(L"UnitAbbreviation_Second"), 3 });
    timeUnits.push_back(
        OrderedUnit{ UnitConverterUnits::Time_Week, GetLocalizedStringName(L"UnitName_Week"), GetLocalizedStringName(L"UnitAbbreviation_Week"), 7 });
    timeUnits.push_back(
        OrderedUnit{ UnitConverterUnits::Time_Year, GetLocalizedStringName(L"UnitName_Year"), GetLocalizedStringName(L"UnitAbbreviation_Year"), 8 });
    unitMap.emplace(ViewMode::Time, timeUnits);

    vector<OrderedUnit> speedUnits;
    speedUnits.push_back(OrderedUnit{ UnitConverterUnits::Speed_CentimetersPerSecond,
                                      GetLocalizedStringName(L"UnitName_CentimetersPerSecond"),
                                      GetLocalizedStringName(L"UnitAbbreviation_CentimetersPerSecond"),
                                      1 });
    speedUnits.push_back(OrderedUnit{ UnitConverterUnits::Speed_FeetPerSecond,
                                      GetLocalizedStringName(L"UnitName_FeetPerSecond"),
                                      GetLocalizedStringName(L"UnitAbbreviation_FeetPerSecond"),
                                      4 });
    speedUnits.push_back(OrderedUnit{ UnitConverterUnits::Speed_KilometersPerHour,
                                      GetLocalizedStringName(L"UnitName_KilometersPerHour"),
                                      GetLocalizedStringName(L"UnitAbbreviation_KilometersPerHour"),
                                      3,
                                      useUSCustomary,
                                      useSI,
                                      false });
    speedUnits.push_back(
        OrderedUnit{ UnitConverterUnits::Speed_Knot, GetLocalizedStringName(L"UnitName_Knot"), GetLocalizedStringName(L"UnitAbbreviation_Knot"), 6 });
    speedUnits.push_back(
        OrderedUnit{ UnitConverterUnits::Speed_Mach, GetLocalizedStringName(L"UnitName_Mach"), GetLocalizedStringName(L"UnitAbbreviation_Mach"), 7 });
    speedUnits.push_back(OrderedUnit{ UnitConverterUnits::Speed_MetersPerSecond,
                                      GetLocalizedStringName(L"UnitName_MetersPerSecond"),
                                      GetLocalizedStringName(L"UnitAbbreviation_MetersPerSecond"),
                                      2 });
    speedUnits.push_back(OrderedUnit{ UnitConverterUnits::Speed_MilesPerHour,
                                      GetLocalizedStringName(L"UnitName_MilesPerHour"),
                                      GetLocalizedStringName(L"UnitAbbreviation_MilesPerHour"),
                                      5,
                                      useSI,
                                      useUSCustomary,
                                      false });
    speedUnits.push_back(OrderedUnit{ UnitConverterUnits::Speed_Turtle,
                                      GetLocalizedStringName(L"UnitName_Turtle"),
                                      GetLocalizedStringName(L"UnitAbbreviation_Turtle"),
                                      8,
                                      false,
                                      false,
                                      true });
    speedUnits.push_back(OrderedUnit{
        UnitConverterUnits::Speed_Horse, GetLocalizedStringName(L"UnitName_Horse"), GetLocalizedStringName(L"UnitAbbreviation_Horse"), 9, false, false, true });
    speedUnits.push_back(OrderedUnit{
        UnitConverterUnits::Speed_Jet, GetLocalizedStringName(L"UnitName_Jet"), GetLocalizedStringName(L"UnitAbbreviation_Jet"), 10, false, false, true });
    unitMap.emplace(ViewMode::Speed, speedUnits);

    vector<OrderedUnit> volumeUnits;
    volumeUnits.push_back(OrderedUnit{ UnitConverterUnits::Volume_CubicCentimeter,
                                       GetLocalizedStringName(L"UnitName_CubicCentimeter"),
                                       GetLocalizedStringName(L"UnitAbbreviation_CubicCentimeter"),
                                       2 });
    volumeUnits.push_back(OrderedUnit{
        UnitConverterUnits::Volume_CubicFoot, GetLocalizedStringName(L"UnitName_CubicFoot"), GetLocalizedStringName(L"UnitAbbreviation_CubicFoot"), 13 });
    volumeUnits.push_back(OrderedUnit{
        UnitConverterUnits::Volume_CubicInch, GetLocalizedStringName(L"UnitName_CubicInch"), GetLocalizedStringName(L"UnitAbbreviation_CubicInch"), 12 });
    volumeUnits.push_back(OrderedUnit{
        UnitConverterUnits::Volume_CubicMeter, GetLocalizedStringName(L"UnitName_CubicMeter"), GetLocalizedStringName(L"UnitAbbreviation_CubicMeter"), 4 });
    volumeUnits.push_back(OrderedUnit{
        UnitConverterUnits::Volume_CubicYard, GetLocalizedStringName(L"UnitName_CubicYard"), GetLocalizedStringName(L"UnitAbbreviation_CubicYard"), 14 });
    volumeUnits.push_back(
        OrderedUnit{ UnitConverterUnits::Volume_CupUS, GetLocalizedStringName(L"UnitName_CupUS"), GetLocalizedStringName(L"UnitAbbreviation_CupUS"), 8 });
    volumeUnits.push_back(OrderedUnit{ UnitConverterUnits::Volume_FluidOunceUK,
                                       GetLocalizedStringName(L"UnitName_FluidOunceUK"),
                                       GetLocalizedStringName(L"UnitAbbreviation_FluidOunceUK"),
                                       17 });
    volumeUnits.push_back(OrderedUnit{ UnitConverterUnits::Volume_FluidOunceUS,
                                       GetLocalizedStringName(L"UnitName_FluidOunceUS"),
                                       GetLocalizedStringName(L"UnitAbbreviation_FluidOunceUS"),
                                       7 });
    volumeUnits.push_back(OrderedUnit{
        UnitConverterUnits::Volume_GallonUK, GetLocalizedStringName(L"UnitName_GallonUK"), GetLocalizedStringName(L"UnitAbbreviation_GallonUK"), 20 });
    volumeUnits.push_back(OrderedUnit{
        UnitConverterUnits::Volume_GallonUS, GetLocalizedStringName(L"UnitName_GallonUS"), GetLocalizedStringName(L"UnitAbbreviation_GallonUS"), 11 });
    volumeUnits.push_back(
        OrderedUnit{ UnitConverterUnits::Volume_Liter, GetLocalizedStringName(L"UnitName_Liter"), GetLocalizedStringName(L"UnitAbbreviation_Liter"), 3 });
    volumeUnits.push_back(OrderedUnit{ UnitConverterUnits::Volume_Milliliter,
                                       GetLocalizedStringName(L"UnitName_Milliliter"),
                                       GetLocalizedStringName(L"UnitAbbreviation_Milliliter"),
                                       1,
                                       useUSCustomary,
                                       useSI });
    volumeUnits.push_back(
        OrderedUnit{ UnitConverterUnits::Volume_PintUK, GetLocalizedStringName(L"UnitName_PintUK"), GetLocalizedStringName(L"UnitAbbreviation_PintUK"), 18 });
    volumeUnits.push_back(
        OrderedUnit{ UnitConverterUnits::Volume_PintUS, GetLocalizedStringName(L"UnitName_PintUS"), GetLocalizedStringName(L"UnitAbbreviation_PintUS"), 9 });
    volumeUnits.push_back(OrderedUnit{ UnitConverterUnits::Volume_TablespoonUS,
                                       GetLocalizedStringName(L"UnitName_TablespoonUS"),
                                       GetLocalizedStringName(L"UnitAbbreviation_TablespoonUS"),
                                       6 });
    volumeUnits.push_back(OrderedUnit{ UnitConverterUnits::Volume_TeaspoonUS,
                                       GetLocalizedStringName(L"UnitName_TeaspoonUS"),
                                       GetLocalizedStringName(L"UnitAbbreviation_TeaspoonUS"),
                                       5,
                                       useSI,
                                       useUSCustomary && m_currentRegionCode != "GB" });
    volumeUnits.push_back(OrderedUnit{
        UnitConverterUnits::Volume_QuartUK, GetLocalizedStringName(L"UnitName_QuartUK"), GetLocalizedStringName(L"UnitAbbreviation_QuartUK"), 19 });
    volumeUnits.push_back(OrderedUnit{
        UnitConverterUnits::Volume_QuartUS, GetLocalizedStringName(L"UnitName_QuartUS"), GetLocalizedStringName(L"UnitAbbreviation_QuartUS"), 10 });
    volumeUnits.push_back(OrderedUnit{ UnitConverterUnits::Volume_TeaspoonUK,
                                       GetLocalizedStringName(L"UnitName_TeaspoonUK"),
                                       GetLocalizedStringName(L"UnitAbbreviation_TeaspoonUK"),
                                       15,
                                       false,
                                       useUSCustomary && m_currentRegionCode == "GB" });
    volumeUnits.push_back(OrderedUnit{ UnitConverterUnits::Volume_TablespoonUK,
                                       GetLocalizedStringName(L"UnitName_TablespoonUK"),
                                       GetLocalizedStringName(L"UnitAbbreviation_TablespoonUK"),
                                       16 });
    volumeUnits.push_back(OrderedUnit{ UnitConverterUnits::Volume_CoffeeCup,
                                       GetLocalizedStringName(L"UnitName_CoffeeCup"),
                                       GetLocalizedStringName(L"UnitAbbreviation_CoffeeCup"),
                                       22,
                                       false,
                                       false,
                                       true });
    volumeUnits.push_back(OrderedUnit{ UnitConverterUnits::Volume_Bathtub,
                                       GetLocalizedStringName(L"UnitName_Bathtub"),
                                       GetLocalizedStringName(L"UnitAbbreviation_Bathtub"),
                                       23,
                                       false,
                                       false,
                                       true });
    volumeUnits.push_back(OrderedUnit{ UnitConverterUnits::Volume_SwimmingPool,
                                       GetLocalizedStringName(L"UnitName_SwimmingPool"),
                                       GetLocalizedStringName(L"UnitAbbreviation_SwimmingPool"),
                                       24,
                                       false,
                                       false,
                                       true });
    unitMap.emplace(ViewMode::Volume, volumeUnits);

    vector<OrderedUnit> weightUnits;
    weightUnits.push_back(
        OrderedUnit{ UnitConverterUnits::Weight_Carat, GetLocalizedStringName(L"UnitName_Carat"), GetLocalizedStringName(L"UnitAbbreviation_Carat"), 1 });
    weightUnits.push_back(OrderedUnit{
        UnitConverterUnits::Weight_Centigram, GetLocalizedStringName(L"UnitName_Centigram"), GetLocalizedStringName(L"UnitAbbreviation_Centigram"), 3 });
    weightUnits.push_back(OrderedUnit{
        UnitConverterUnits::Weight_Decigram, GetLocalizedStringName(L"UnitName_Decigram"), GetLocalizedStringName(L"UnitAbbreviation_Decigram"), 4 });
    weightUnits.push_back(OrderedUnit{
        UnitConverterUnits::Weight_Decagram, GetLocalizedStringName(L"UnitName_Decagram"), GetLocalizedStringName(L"UnitAbbreviation_Decagram"), 6 });
    weightUnits.push_back(
        OrderedUnit{ UnitConverterUnits::Weight_Gram, GetLocalizedStringName(L"UnitName_Gram"), GetLocalizedStringName(L"UnitAbbreviation_Gram"), 5 });
    weightUnits.push_back(OrderedUnit{
        UnitConverterUnits::Weight_Hectogram, GetLocalizedStringName(L"UnitName_Hectogram"), GetLocalizedStringName(L"UnitAbbreviation_Hectogram"), 7 });
    weightUnits.push_back(OrderedUnit{ UnitConverterUnits::Weight_Kilogram,
                                       GetLocalizedStringName(L"UnitName_Kilogram"),
                                       GetLocalizedStringName(L"UnitAbbreviation_Kilogram"),
                                       8,
                                       useUSCustomary,
                                       useSI });
    weightUnits.push_back(OrderedUnit{
        UnitConverterUnits::Weight_LongTon, GetLocalizedStringName(L"UnitName_LongTon"), GetLocalizedStringName(L"UnitAbbreviation_LongTon"), 14 });
    weightUnits.push_back(OrderedUnit{
        UnitConverterUnits::Weight_Milligram, GetLocalizedStringName(L"UnitName_Milligram"), GetLocalizedStringName(L"UnitAbbreviation_Milligram"), 2 });
    weightUnits.push_back(
        OrderedUnit{ UnitConverterUnits::Weight_Ounce, GetLocalizedStringName(L"UnitName_Ounce"), GetLocalizedStringName(L"UnitAbbreviation_Ounce"), 10 });
    weightUnits.push_back(OrderedUnit{ UnitConverterUnits::Weight_Pound,
                                       GetLocalizedStringName(L"UnitName_Pound"),
                                       GetLocalizedStringName(L"UnitAbbreviation_Pound"),
                                       11,
                                       useSI,
                                       useUSCustomary });
    weightUnits.push_back(OrderedUnit{
        UnitConverterUnits::Weight_ShortTon, GetLocalizedStringName(L"UnitName_ShortTon"), GetLocalizedStringName(L"UnitAbbreviation_ShortTon"), 13 });
    weightUnits.push_back(
        OrderedUnit{ UnitConverterUnits::Weight_Stone, GetLocalizedStringName(L"UnitName_Stone"), GetLocalizedStringName(L"UnitAbbreviation_Stone"), 12 });
    weightUnits.push_back(
        OrderedUnit{ UnitConverterUnits::Weight_Tonne, GetLocalizedStringName(L"UnitName_Tonne"), GetLocalizedStringName(L"UnitAbbreviation_Tonne"), 9 });
    weightUnits.push_back(OrderedUnit{ UnitConverterUnits::Weight_Snowflake,
                                       GetLocalizedStringName(L"UnitName_Snowflake"),
                                       GetLocalizedStringName(L"UnitAbbreviation_Snowflake"),
                                       15,
                                       false,
                                       false,
                                       true });
    weightUnits.push_back(OrderedUnit{ UnitConverterUnits::Weight_SoccerBall,
                                       GetLocalizedStringName(L"UnitName_SoccerBall"),
                                       GetLocalizedStringName(L"UnitAbbreviation_SoccerBall"),
                                       16,
                                       false,
                                       false,
                                       true });
    weightUnits.push_back(OrderedUnit{ UnitConverterUnits::Weight_Elephant,
                                       GetLocalizedStringName(L"UnitName_Elephant"),
                                       GetLocalizedStringName(L"UnitAbbreviation_Elephant"),
                                       17,
                                       false,
                                       false,
                                       true });
    weightUnits.push_back(OrderedUnit{ UnitConverterUnits::Weight_Whale,
                                       GetLocalizedStringName(L"UnitName_Whale"),
                                       GetLocalizedStringName(L"UnitAbbreviation_Whale"),
                                       18,
                                       false,
                                       false,
                                       true });
    unitMap.emplace(ViewMode::Weight, weightUnits);

    vector<OrderedUnit> pressureUnits;
    pressureUnits.push_back(OrderedUnit{ UnitConverterUnits::Pressure_Atmosphere,
                                         GetLocalizedStringName(L"UnitName_Atmosphere"),
                                         GetLocalizedStringName(L"UnitAbbreviation_Atmosphere"),
                                         1,
                                         true,
                                         false,
                                         false });
    pressureUnits.push_back(OrderedUnit{
        UnitConverterUnits::Pressure_Bar, GetLocalizedStringName(L"UnitName_Bar"), GetLocalizedStringName(L"UnitAbbreviation_Bar"), 2, false, true, false });
    pressureUnits.push_back(OrderedUnit{
        UnitConverterUnits::Pressure_KiloPascal, GetLocalizedStringName(L"UnitName_KiloPascal"), GetLocalizedStringName(L"UnitAbbreviation_KiloPascal"), 3 });
    pressureUnits.push_back(OrderedUnit{ UnitConverterUnits::Pressure_MillimeterOfMercury,
                                         GetLocalizedStringName(L"UnitName_MillimeterOfMercury "),
                                         GetLocalizedStringName(L"UnitAbbreviation_MillimeterOfMercury "),
                                         4 });
    pressureUnits.push_back(
        OrderedUnit{ UnitConverterUnits::Pressure_Pascal, GetLocalizedStringName(L"UnitName_Pascal"), GetLocalizedStringName(L"UnitAbbreviation_Pascal"), 5 });
    pressureUnits.push_back(OrderedUnit{
        UnitConverterUnits::Pressure_PSI, GetLocalizedStringName(L"UnitName_PSI"), GetLocalizedStringName(L"UnitAbbreviation_PSI"), 6, false, false, false });
    unitMap.emplace(ViewMode::Pressure, pressureUnits);

    vector<OrderedUnit> angleUnits;
    angleUnits.push_back(OrderedUnit{ UnitConverterUnits::Angle_Degree,
                                      GetLocalizedStringName(L"UnitName_Degree"),
                                      GetLocalizedStringName(L"UnitAbbreviation_Degree"),
                                      1,
                                      true,
                                      false,
                                      false });
    angleUnits.push_back(OrderedUnit{ UnitConverterUnits::Angle_Radian,
                                      GetLocalizedStringName(L"UnitName_Radian"),
                                      GetLocalizedStringName(L"UnitAbbreviation_Radian"),
                                      2,
                                      false,
                                      true,
                                      false });
    angleUnits.push_back(
        OrderedUnit{ UnitConverterUnits::Angle_Gradian, GetLocalizedStringName(L"UnitName_Gradian"), GetLocalizedStringName(L"UnitAbbreviation_Gradian"), 3 });
    unitMap.emplace(ViewMode::Angle, angleUnits);
}

PRAT rat_pressure_bar = nullptr;
PRAT rat_pressure_pascal = nullptr;
PRAT rat_pressure_kilopascal = nullptr;
PRAT rat_pressure_psi = nullptr;
PRAT rat_power_horsepower = nullptr;
PRAT rat_power_horse = nullptr;
PRAT rat_length_inch = nullptr;
PRAT rat_length_yard = nullptr;
PRAT rat_length_mile = nullptr;
PRAT rat_length_nauticalmile = nullptr;
PRAT rat_length_paperclip = nullptr;
PRAT rat_length_hand = nullptr;
PRAT rat_length_jumbojet = nullptr;
PRAT rat_speed_kilometersperhour = nullptr;
PRAT rat_energy_electronvolt = nullptr;
PRAT rat_0_000000001 = nullptr;
PRAT rat_0_000000125 = nullptr;
PRAT rat_0_000001 = nullptr;
PRAT rat_0_000002 = nullptr;
PRAT rat_0_00001 = nullptr;
PRAT rat_0_0001 = nullptr;
PRAT rat_0_000125 = nullptr;
PRAT rat_0_000128 = nullptr;
PRAT rat_0_0002 = nullptr;
PRAT rat_area_squareinch = nullptr;
PRAT rat_0_001 = nullptr;
PRAT rat_0_001024 = nullptr;
PRAT rat_pressure_millimeterofmercury = nullptr;
PRAT rat_0_0098692326671601 = nullptr;
PRAT rat_0_01 = nullptr;
PRAT rat_0_012516104 = nullptr;
PRAT rat_0_0225969658055233 = nullptr;
PRAT rat_0_028349523125 = nullptr;
PRAT rat_0_035052 = nullptr;
PRAT rat_0_06032246 = nullptr;
PRAT rat_0_068045961016531 = nullptr;
PRAT rat_0_09290304 = nullptr;
PRAT rat_0_1 = nullptr;
PRAT rat_0_125 = nullptr;
PRAT rat_0_131072 = nullptr;
PRAT rat_0_18669 = nullptr;
PRAT rat_0_3048 = nullptr;
PRAT rat_0_4325 = nullptr;
PRAT rat_0_45359237 = nullptr;
PRAT rat_0_83612736 = nullptr;
PRAT rat_0_9 = nullptr;
PRAT rat_0_9144 = nullptr;
PRAT rat_100 = nullptr;
PRAT rat_1000 = nullptr;
PRAT rat_10000 = nullptr;
PRAT rat_100000 = nullptr;
PRAT rat_1000000 = nullptr;
PRAT rat_60 = nullptr;
PRAT rat_one = nullptr;

INIT_AND_DUMP_RAW_RAT_FRAQ_IF_NULL(rat, num, div) \
    if (rat == nullptr)            \
    {                              \
        createrat(rat);            \
        DUPNUM(rat_half->pp, num); \
        DUPNUM(rat_half->pq, div); \
        DUMPRAWRAT(rat);           \
    }

void UnitConverterDataLoader::GetConversionData(_In_ unordered_map<ViewMode, unordered_map<int, double>>& categoryToUnitConversionMap)
{
    INIT_AND_DUMP_RAW_RAT_IF_NULL(rat_60, 60);
    INIT_AND_DUMP_RAW_RAT_IF_NULL(rat_100, 100);
    INIT_AND_DUMP_RAW_RAT_IF_NULL(rat_1000, 1000);
    INIT_AND_DUMP_RAW_RAT_IF_NULL(rat_10000, 10000);
    INIT_AND_DUMP_RAW_RAT_IF_NULL(rat_100000, 100000);
    INIT_AND_DUMP_RAW_RAT_IF_NULL(rat_1000000, 1000000);
    INIT_AND_DUMP_RAW_RAT_FRAQ_IF_NULL(rat_pressure_bar, 100000, 101325);
    INIT_AND_DUMP_RAW_RAT_FRAQ_IF_NULL(rat_pressure_pascal, 1, 101325);
    INIT_AND_DUMP_RAW_RAT_FRAQ_IF_NULL(rat_pressure_kilopascal, 1000, 101325);
    INIT_AND_DUMP_RAW_RAT_FRAQ_IF_NULL(rat_power_horsepower, 74569987158227022, 100000000000000);
    INIT_AND_DUMP_RAW_RAT_FRAQ_IF_NULL(rat_power_horse, 7457, 10);
    INIT_AND_DUMP_RAW_RAT_FRAQ_IF_NULL(rat_pressure_millimeterofmercury, 1, 760);
    INIT_AND_DUMP_RAW_RAT_FRAQ_IF_NULL(rat_pressure_psi, 10000, 146956);
    INIT_AND_DUMP_RAW_RAT_FRAQ_IF_NULL(rat_length_inch, 254, 10000);
    INIT_AND_DUMP_RAW_RAT_FRAQ_IF_NULL(rat_foot, 3048, 10000);
    INIT_AND_DUMP_RAW_RAT_FRAQ_IF_NULL(rat_length_yard, 9144, 10000);
    INIT_AND_DUMP_RAW_RAT_FRAQ_IF_NULL(rat_length_mile, 1609344, 1000);
    INIT_AND_DUMP_RAW_RAT_FRAQ_IF_NULL(rat_length_nauticalmile, 1852, 1);
    INIT_AND_DUMP_RAW_RAT_FRAQ_IF_NULL(rat_length_paperclip, 35052, 1000000);
    INIT_AND_DUMP_RAW_RAT_FRAQ_IF_NULL(rat_length_hand, 18669, 100000);
    INIT_AND_DUMP_RAW_RAT_FRAQ_IF_NULL(rat_speed_kilometersperhour, 250, 9);
    INIT_AND_DUMP_RAW_RAT_IF_NULL(rat_length_jumbojet, 76);
    INIT_AND_DUMP_RAW_RAT_FRAQ_IF_NULL(rat_0_125, 1, 8);
    INIT_AND_DUMP_RAW_RAT_FRAQ_IF_NULL(rat_0_000125, 1, 8000);
    INIT_AND_DUMP_RAW_RAT_FRAQ_IF_NULL(rat_0_000000125, 1, 8000000);
    INIT_AND_DUMP_RAW_RAT_FRAQ_IF_NULL(rat_0_000000001, 1, 100000000);
    INIT_AND_DUMP_RAW_RAT_FRAQ_IF_NULL(rat_0_000002, 2, 100000);
    INIT_AND_DUMP_RAW_RAT_FRAQ_IF_NULL(rat_0_000001, 1, 100000);
    INIT_AND_DUMP_RAW_RAT_FRAQ_IF_NULL(rat_0_00001, 1, 10000);
    INIT_AND_DUMP_RAW_RAT_FRAQ_IF_NULL(rat_0_0002, 2, 10000);
    INIT_AND_DUMP_RAW_RAT_FRAQ_IF_NULL(rat_0_0001, 1, 10000);
    INIT_AND_DUMP_RAW_RAT_FRAQ_IF_NULL(rat_0_001, 1, 1000);
    INIT_AND_DUMP_RAW_RAT_FRAQ_IF_NULL(rat_0_01, 1, 100);
    INIT_AND_DUMP_RAW_RAT_FRAQ_IF_NULL(rat_0_1, 1, 10);
    INIT_AND_DUMP_RAW_RAT_FRAQ_IF_NULL(rat_0_9, 9, 10);
    /*categoryId, UnitId, factor*/
    static const vector<UnitData> unitDataList = { { ViewMode::Area, UnitConverterUnits::Area_Acre, rat_area_acre },
                                                   { ViewMode::Area, UnitConverterUnits::Area_SquareMeter, rat_one },
                                                   { ViewMode::Area, UnitConverterUnits::Area_SquareFoot, rat_area_squarefoot },
                                                   { ViewMode::Area, UnitConverterUnits::Area_SquareYard, rat_area_squareyard },
                                                   { ViewMode::Area, UnitConverterUnits::Area_SquareMillimeter, rat_area_squaremillimeter },
                                                   { ViewMode::Area, UnitConverterUnits::Area_SquareCentimeter, rat_area_squarecentimeter },
                                                   { ViewMode::Area, UnitConverterUnits::Area_SquareInch, rat_area_squareinch },
                                                   { ViewMode::Area, UnitConverterUnits::Area_SquareMile, rat_area_squaremile },
                                                   { ViewMode::Area, UnitConverterUnits::Area_SquareKilometer, rat_1000000 },
                                                   { ViewMode::Area, UnitConverterUnits::Area_Hectare, rat_10000 },
                                                   { ViewMode::Area, UnitConverterUnits::Area_Hand, rat_area_hand },
                                                   { ViewMode::Area, UnitConverterUnits::Area_Paper, rat_area_paper },
                                                   { ViewMode::Area, UnitConverterUnits::Area_SoccerField, rat_area_soccerfield },
                                                   { ViewMode::Area, UnitConverterUnits::Area_Castle, rat_100000 },
                                                   { ViewMode::Area, UnitConverterUnits::Area_Pyeong, rat_area_pyeong / 121.0 },

                                                   { ViewMode::Data, UnitConverterUnits::Data_Bit, rat_0_000000125 },
                                                   { ViewMode::Data, UnitConverterUnits::Data_Byte, rat_0_000001 },
                                                   { ViewMode::Data, UnitConverterUnits::Data_Kilobyte, rat_0_001 },
                                                   { ViewMode::Data, UnitConverterUnits::Data_Megabyte, rat_one },
                                                   { ViewMode::Data, UnitConverterUnits::Data_Gigabyte, rat_1000 },
                                                   { ViewMode::Data, UnitConverterUnits::Data_Terabyte, rat_1000000 },
                                                   { ViewMode::Data, UnitConverterUnits::Data_Petabyte, 1000000000 },
                                                   { ViewMode::Data, UnitConverterUnits::Data_Exabytes, 1000000000000 },
                                                   { ViewMode::Data, UnitConverterUnits::Data_Zetabytes, 1000000000000000 },
                                                   { ViewMode::Data, UnitConverterUnits::Data_Yottabyte, 1000000000000000000 },
                                                   { ViewMode::Data, UnitConverterUnits::Data_Kilobit, rat_0_000125 },
                                                   { ViewMode::Data, UnitConverterUnits::Data_Megabit, rat_0_125 },
                                                   { ViewMode::Data, UnitConverterUnits::Data_Gigabit, 125 },
                                                   { ViewMode::Data, UnitConverterUnits::Data_Terabit, 125000 },
                                                   { ViewMode::Data, UnitConverterUnits::Data_Petabit, 125000000 },
                                                   { ViewMode::Data, UnitConverterUnits::Data_Exabits, 125000000000 },
                                                   { ViewMode::Data, UnitConverterUnits::Data_Zetabits, 125000000000000 },
                                                   { ViewMode::Data, UnitConverterUnits::Data_Yottabit, 125000000000000000 },
                                                   { ViewMode::Data, UnitConverterUnits::Data_Gibibits, 134.217728 },
                                                   { ViewMode::Data, UnitConverterUnits::Data_Gibibytes, 1073.741824 },
                                                   { ViewMode::Data, UnitConverterUnits::Data_Kibibits, rat_0_000128 },
                                                   { ViewMode::Data, UnitConverterUnits::Data_Kibibytes, rat_0_001024 },
                                                   { ViewMode::Data, UnitConverterUnits::Data_Mebibits, rat_0_131072 },
                                                   { ViewMode::Data, UnitConverterUnits::Data_Mebibytes, 1.048576 },
                                                   { ViewMode::Data, UnitConverterUnits::Data_Pebibits, 140737488.355328 },
                                                   { ViewMode::Data, UnitConverterUnits::Data_Pebibytes, 1125899906.842624 },
                                                   { ViewMode::Data, UnitConverterUnits::Data_Tebibits, 137438.953472 },
                                                   { ViewMode::Data, UnitConverterUnits::Data_Tebibytes, 1099511.627776 },
                                                   { ViewMode::Data, UnitConverterUnits::Data_Exbibits, 144115188075.855872 },
                                                   { ViewMode::Data, UnitConverterUnits::Data_Exbibytes, 1152921504606.846976 },
                                                   { ViewMode::Data, UnitConverterUnits::Data_Zebibits, 147573952589676.412928 },
                                                   { ViewMode::Data, UnitConverterUnits::Data_Zebibytes, 1180591620717411.303424 },
                                                   { ViewMode::Data, UnitConverterUnits::Data_Yobibits, 151115727451828646.838272 },
                                                   { ViewMode::Data, UnitConverterUnits::Data_Yobibytes, 1208925819614629174.706176 },
                                                   { ViewMode::Data, UnitConverterUnits::Data_FloppyDisk, 1.509949 },
                                                   { ViewMode::Data, UnitConverterUnits::Data_CD, 734.003200 },
                                                   { ViewMode::Data, UnitConverterUnits::Data_DVD, 5046.586573 },

                                                   { ViewMode::Energy, UnitConverterUnits::Energy_Calorie, 4.184 },
                                                   { ViewMode::Energy, UnitConverterUnits::Energy_Kilocalorie, 4184 },
                                                   { ViewMode::Energy, UnitConverterUnits::Energy_BritishThermalUnit, 1055.056 },
                                                   { ViewMode::Energy, UnitConverterUnits::Energy_Kilojoule, rat_1000 },
                                                   { ViewMode::Energy, UnitConverterUnits::Energy_ElectronVolt, rat_energy_electronvolt },
                                                   { ViewMode::Energy, UnitConverterUnits::Energy_Joule, rat_one },
                                                   { ViewMode::Energy, UnitConverterUnits::Energy_FootPound, 1.3558179483314 },
                                                   { ViewMode::Energy, UnitConverterUnits::Energy_Battery, 9000 },
                                                   { ViewMode::Energy, UnitConverterUnits::Energy_Banana, 439614 },
                                                   { ViewMode::Energy, UnitConverterUnits::Energy_SliceOfCake, 1046700 },

                                                   { ViewMode::Length, UnitConverterUnits::Length_Inch, rat_length_inch },
                                                   { ViewMode::Length, UnitConverterUnits::Length_Foot, rat_foot },
                                                   { ViewMode::Length, UnitConverterUnits::Length_Yard, rat_length_yard },
                                                   { ViewMode::Length, UnitConverterUnits::Length_Mile, rat_length_mile },
                                                   { ViewMode::Length, UnitConverterUnits::Length_Micron, rat_0_000001 },
                                                   { ViewMode::Length, UnitConverterUnits::Length_Millimeter, rat_0_001 },
                                                   { ViewMode::Length, UnitConverterUnits::Length_Nanometer, rat_0_000000001 },
                                                   { ViewMode::Length, UnitConverterUnits::Length_Centimeter, rat_0_01 },
                                                   { ViewMode::Length, UnitConverterUnits::Length_Meter, rat_one },
                                                   { ViewMode::Length, UnitConverterUnits::Length_Kilometer, rat_1000 },
                                                   { ViewMode::Length, UnitConverterUnits::Length_NauticalMile, rat_length_nauticalmile },
                                                   { ViewMode::Length, UnitConverterUnits::Length_Paperclip, rat_length_paperclip },
                                                   { ViewMode::Length, UnitConverterUnits::Length_Hand, rat_length_hand },
                                                   { ViewMode::Length, UnitConverterUnits::Length_JumboJet, rat_length_jumbojet },

                                                   { ViewMode::Power, UnitConverterUnits::Power_BritishThermalUnitPerMinute, 17.58426666666667 },
                                                   { ViewMode::Power, UnitConverterUnits::Power_FootPoundPerMinute, rat_0_0225969658055233 },
                                                   { ViewMode::Power, UnitConverterUnits::Power_Watt, rat_one },
                                                   { ViewMode::Power, UnitConverterUnits::Power_Kilowatt, rat_1000 },
                                                   { ViewMode::Power, UnitConverterUnits::Power_Horsepower, rat_power_horsepower },
                                                   { ViewMode::Power, UnitConverterUnits::Power_LightBulb, rat_60 },
                                                   { ViewMode::Power, UnitConverterUnits::Power_Horse, rat_power_horse },
                                                   { ViewMode::Power, UnitConverterUnits::Power_TrainEngine, 2982799.486329081 },

                                                   { ViewMode::Time, UnitConverterUnits::Time_Day, 86400 },
                                                   { ViewMode::Time, UnitConverterUnits::Time_Second, rat_one },
                                                   { ViewMode::Time, UnitConverterUnits::Time_Week, 604800 },
                                                   { ViewMode::Time, UnitConverterUnits::Time_Year, 31557600 },
                                                   { ViewMode::Time, UnitConverterUnits::Time_Millisecond, rat_0_001 },
                                                   { ViewMode::Time, UnitConverterUnits::Time_Microsecond, rat_0_000001 },
                                                   { ViewMode::Time, UnitConverterUnits::Time_Minute, rat_60 },
                                                   { ViewMode::Time, UnitConverterUnits::Time_Hour, 3600 },

                                                   { ViewMode::Volume, UnitConverterUnits::Volume_CupUS, 236.588237 },
                                                   { ViewMode::Volume, UnitConverterUnits::Volume_PintUS, 473.176473 },
                                                   { ViewMode::Volume, UnitConverterUnits::Volume_PintUK, 568.26125 },
                                                   { ViewMode::Volume, UnitConverterUnits::Volume_QuartUS, 946.352946 },
                                                   { ViewMode::Volume, UnitConverterUnits::Volume_QuartUK, 1136.5225 },
                                                   { ViewMode::Volume, UnitConverterUnits::Volume_GallonUS, 3785.411784 },
                                                   { ViewMode::Volume, UnitConverterUnits::Volume_GallonUK, 4546.09 },
                                                   { ViewMode::Volume, UnitConverterUnits::Volume_Liter, rat_1000 },
                                                   { ViewMode::Volume, UnitConverterUnits::Volume_TeaspoonUS, 4.92892159375 },
                                                   { ViewMode::Volume, UnitConverterUnits::Volume_TablespoonUS, 14.78676478125 },
                                                   { ViewMode::Volume, UnitConverterUnits::Volume_CubicCentimeter, rat_one },
                                                   { ViewMode::Volume, UnitConverterUnits::Volume_CubicYard, 764554.857984 },
                                                   { ViewMode::Volume, UnitConverterUnits::Volume_CubicMeter, rat_1000000 },
                                                   { ViewMode::Volume, UnitConverterUnits::Volume_Milliliter, rat_one },
                                                   { ViewMode::Volume, UnitConverterUnits::Volume_CubicInch, 16.387064 },
                                                   { ViewMode::Volume, UnitConverterUnits::Volume_CubicFoot, 28316.846592 },
                                                   { ViewMode::Volume, UnitConverterUnits::Volume_FluidOunceUS, 29.5735295625 },
                                                   { ViewMode::Volume, UnitConverterUnits::Volume_FluidOunceUK, 28.4130625 },
                                                   { ViewMode::Volume, UnitConverterUnits::Volume_TeaspoonUK, 5.91938802083333333333 },
                                                   { ViewMode::Volume, UnitConverterUnits::Volume_TablespoonUK, 17.7581640625 },
                                                   { ViewMode::Volume, UnitConverterUnits::Volume_CoffeeCup, 236.5882 },
                                                   { ViewMode::Volume, UnitConverterUnits::Volume_Bathtub, 378541.2 },
                                                   { ViewMode::Volume, UnitConverterUnits::Volume_SwimmingPool, 3750000000 },

                                                   { ViewMode::Weight, UnitConverterUnits::Weight_Kilogram, rat_one },
                                                   { ViewMode::Weight, UnitConverterUnits::Weight_Hectogram, rat_0_1 },
                                                   { ViewMode::Weight, UnitConverterUnits::Weight_Decagram, rat_0_01 },
                                                   { ViewMode::Weight, UnitConverterUnits::Weight_Gram, rat_0_001 },
                                                   { ViewMode::Weight, UnitConverterUnits::Weight_Pound, rat_0_45359237 },
                                                   { ViewMode::Weight, UnitConverterUnits::Weight_Ounce, rat_0_028349523125 },
                                                   { ViewMode::Weight, UnitConverterUnits::Weight_Milligram, rat_0_000001 },
                                                   { ViewMode::Weight, UnitConverterUnits::Weight_Centigram, rat_0_00001 },
                                                   { ViewMode::Weight, UnitConverterUnits::Weight_Decigram, rat_0_0001 },
                                                   { ViewMode::Weight, UnitConverterUnits::Weight_LongTon, 1016.0469088 },
                                                   { ViewMode::Weight, UnitConverterUnits::Weight_Tonne, rat_1000 },
                                                   { ViewMode::Weight, UnitConverterUnits::Weight_Stone, 6.35029318 },
                                                   { ViewMode::Weight, UnitConverterUnits::Weight_Carat, rat_0_0002 },
                                                   { ViewMode::Weight, UnitConverterUnits::Weight_ShortTon, 907.18474 },
                                                   { ViewMode::Weight, UnitConverterUnits::Weight_Snowflake, rat_0_000002 },
                                                   { ViewMode::Weight, UnitConverterUnits::Weight_SoccerBall, rat_0_4325 },
                                                   { ViewMode::Weight, UnitConverterUnits::Weight_Elephant, 4000 },
                                                   { ViewMode::Weight, UnitConverterUnits::Weight_Whale, 90000 },

                                                   { ViewMode::Speed, UnitConverterUnits::Speed_CentimetersPerSecond, rat_one },
                                                   { ViewMode::Speed, UnitConverterUnits::Speed_FeetPerSecond, 30.48 },
                                                   { ViewMode::Speed, UnitConverterUnits::Speed_KilometersPerHour, rat_speed_kilometersperhour },
                                                   { ViewMode::Speed, UnitConverterUnits::Speed_Knot, 51.44 },
                                                   { ViewMode::Speed, UnitConverterUnits::Speed_Mach, 34030 },
                                                   { ViewMode::Speed, UnitConverterUnits::Speed_MetersPerSecond, rat_100 },
                                                   { ViewMode::Speed, UnitConverterUnits::Speed_MilesPerHour, 44.7 },
                                                   { ViewMode::Speed, UnitConverterUnits::Speed_Turtle, 8.94 },
                                                   { ViewMode::Speed, UnitConverterUnits::Speed_Horse, 2011.5 },
                                                   { ViewMode::Speed, UnitConverterUnits::Speed_Jet, 24585 },

                                                   { ViewMode::Angle, UnitConverterUnits::Angle_Degree, rat_one },
                                                   { ViewMode::Angle, UnitConverterUnits::Angle_Radian, 57.29577951308233 },
                                                   { ViewMode::Angle, UnitConverterUnits::Angle_Gradian, rat_0_9 },

                                                   { ViewMode::Pressure, UnitConverterUnits::Pressure_Atmosphere, rat_one },
                                                   { ViewMode::Pressure, UnitConverterUnits::Pressure_Bar, rat_pressure_bar },
                                                   { ViewMode::Pressure, UnitConverterUnits::Pressure_KiloPascal, rat_pressure_kilopascal },
                                                   { ViewMode::Pressure, UnitConverterUnits::Pressure_MillimeterOfMercury, rat_pressure_millimeterofmercury },
                                                   { ViewMode::Pressure, UnitConverterUnits::Pressure_Pascal, rat_pressure_pascal },
                                                   { ViewMode::Pressure, UnitConverterUnits::Pressure_PSI, rat_pressure_psi } };

    // Populate the hash map and return;
    for (UnitData unitdata : unitDataList)
    {
        if (categoryToUnitConversionMap.find(unitdata.categoryId) == categoryToUnitConversionMap.end())
        {
            unordered_map<int, PRAT> conversionData;
            conversionData.insert(pair<int, PRAT>(unitdata.unitId, unitdata.factor));
            categoryToUnitConversionMap.insert(pair<ViewMode, unordered_map<int, PRAT>>(unitdata.categoryId, conversionData));
        }
        else
        {
            categoryToUnitConversionMap.at(unitdata.categoryId).insert(pair<int, PRAT>(unitdata.unitId, unitdata.factor));
        }
    }
}

wstring UnitConverterDataLoader::GetLocalizedStringName(String ^ stringId)
{
    return AppResourceProvider::GetInstance().GetResourceString(stringId)->Data();
}

void UnitConverterDataLoader::GetExplicitConversionData(_In_ unordered_map<int, unordered_map<int, UCM::ConversionData>>& unitToUnitConversionList)
{
    /* categoryId, ParentUnitId, UnitId, ratio, offset, offsetfirst*/
    ExplicitUnitConversionData conversionDataList[] = {
        { ViewMode::Temperature, UnitConverterUnits::Temperature_DegreesCelsius, UnitConverterUnits::Temperature_DegreesCelsius, 1, 0 },
        { ViewMode::Temperature, UnitConverterUnits::Temperature_DegreesCelsius, UnitConverterUnits::Temperature_DegreesFahrenheit, 1.8, 32 },
        { ViewMode::Temperature, UnitConverterUnits::Temperature_DegreesCelsius, UnitConverterUnits::Temperature_Kelvin, 1, 273.15 },
        { ViewMode::Temperature,
          UnitConverterUnits::Temperature_DegreesFahrenheit,
          UnitConverterUnits::Temperature_DegreesCelsius,
          0.55555555555555555555555555555556,
          -32,
          CONVERT_WITH_OFFSET_FIRST },
        { ViewMode::Temperature, UnitConverterUnits::Temperature_DegreesFahrenheit, UnitConverterUnits::Temperature_DegreesFahrenheit, 1, 0 },
        { ViewMode::Temperature,
          UnitConverterUnits::Temperature_DegreesFahrenheit,
          UnitConverterUnits::Temperature_Kelvin,
          0.55555555555555555555555555555556,
          459.67,
          CONVERT_WITH_OFFSET_FIRST },
        { ViewMode::Temperature,
          UnitConverterUnits::Temperature_Kelvin,
          UnitConverterUnits::Temperature_DegreesCelsius,
          1,
          -273.15,
          CONVERT_WITH_OFFSET_FIRST },
        { ViewMode::Temperature, UnitConverterUnits::Temperature_Kelvin, UnitConverterUnits::Temperature_DegreesFahrenheit, 1.8, -459.67 },
        { ViewMode::Temperature, UnitConverterUnits::Temperature_Kelvin, UnitConverterUnits::Temperature_Kelvin, 1, 0 }
    };

    // Populate the hash map and return;
    for (ExplicitUnitConversionData data : conversionDataList)
    {
        if (unitToUnitConversionList.find(data.parentUnitId) == unitToUnitConversionList.end())
        {
            unordered_map<int, UCM::ConversionData> conversionData;
            conversionData.insert(pair<int, UCM::ConversionData>(data.unitId, static_cast<UCM::ConversionData>(data)));
            unitToUnitConversionList.insert(pair<int, unordered_map<int, UCM::ConversionData>>(data.parentUnitId, conversionData));
        }
        else
        {
            unitToUnitConversionList.at(data.parentUnitId).insert(pair<int, UCM::ConversionData>(data.unitId, static_cast<UCM::ConversionData>(data)));
        }
    }
}
