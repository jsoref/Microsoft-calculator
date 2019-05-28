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
    unordered_map<ViewMode, unordered_map<int, PRAT>> categoryToUnitConversionDataMap{};
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
                unordered_map<int, PRAT> unitConversions = categoryToUnitConversionDataMap.at(categoryViewMode);
                PRAT unitFactor = unitConversions[unit.id];

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

PRAT rat_0_000001 = nullptr;
PRAT rat_0_000002 = nullptr;
PRAT rat_0_00001 = nullptr;
PRAT rat_0_0001 = nullptr;
PRAT rat_0_0002 = nullptr;
PRAT rat_0_001 = nullptr;
PRAT rat_0_01 = nullptr;
PRAT rat_0_1 = nullptr;
PRAT rat_100 = nullptr;
PRAT rat_1000 = nullptr;
PRAT rat_10000 = nullptr;
PRAT rat_100000 = nullptr;
PRAT rat_1000000 = nullptr;
PRAT rat_60 = nullptr;
PRAT rat_angle_gradian = nullptr;
PRAT rat_angle_radian = nullptr;
PRAT rat_area_acre = nullptr;
PRAT rat_area_hand = nullptr;
PRAT rat_area_paper = nullptr;
PRAT rat_area_pyeong = nullptr;
PRAT rat_area_soccerfield = nullptr;
PRAT rat_area_squarefoot = nullptr;
PRAT rat_area_squareinch = nullptr;
PRAT rat_area_squaremile = nullptr;
PRAT rat_area_squareyard = nullptr;
PRAT rat_data_bit = nullptr;
PRAT rat_data_cd = nullptr;
PRAT rat_data_dvd = nullptr;
PRAT rat_data_exabits = nullptr;
PRAT rat_data_exabytes = nullptr;
PRAT rat_data_exbibits = nullptr;
PRAT rat_data_exbibytes = nullptr;
PRAT rat_data_floppydisk = nullptr;
PRAT rat_data_gibibits = nullptr;
PRAT rat_data_gibibytes = nullptr;
PRAT rat_data_gigabit = nullptr;
PRAT rat_data_kibibits = nullptr;
PRAT rat_data_kibibytes = nullptr;
PRAT rat_data_kilobit = nullptr;
PRAT rat_data_mebibits = nullptr;
PRAT rat_data_mebibytes = nullptr;
PRAT rat_data_pebibits = nullptr;
PRAT rat_data_pebibytes = nullptr;
PRAT rat_data_petabit = nullptr;
PRAT rat_data_petabyte = nullptr;
PRAT rat_data_tebibits = nullptr;
PRAT rat_data_tebibytes = nullptr;
PRAT rat_data_terabit = nullptr;
PRAT rat_data_yobibits = nullptr;
PRAT rat_data_yobibytes = nullptr;
PRAT rat_data_yottabit = nullptr;
PRAT rat_data_yottabyte = nullptr;
PRAT rat_data_zebibits = nullptr;
PRAT rat_data_zebibytes = nullptr;
PRAT rat_data_zetabits = nullptr;
PRAT rat_data_zetabytes = nullptr;
PRAT rat_energy_banana = nullptr;
PRAT rat_energy_battery = nullptr;
PRAT rat_energy_britishthermalunit = nullptr;
PRAT rat_energy_calorie = nullptr;
PRAT rat_energy_electronvolt = nullptr;
PRAT rat_energy_footpound = nullptr;
PRAT rat_energy_kilocalorie = nullptr;
PRAT rat_energy_sliceofcake = nullptr;
PRAT rat_foot = nullptr;
PRAT rat_length_hand = nullptr;
PRAT rat_length_inch = nullptr;
PRAT rat_length_jumbojet = nullptr;
PRAT rat_length_mile = nullptr;
PRAT rat_length_nanometer = nullptr;
PRAT rat_length_nauticalmile = nullptr;
PRAT rat_length_paperclip = nullptr;
PRAT rat_length_yard = nullptr;
PRAT rat_power_britishthermalunitperminute = nullptr;
PRAT rat_power_footpoundperminute = nullptr;
PRAT rat_power_horse = nullptr;
PRAT rat_power_horsepower = nullptr;
PRAT rat_power_trainengine = nullptr;
PRAT rat_pressure_bar = nullptr;
PRAT rat_pressure_kilopascal = nullptr;
PRAT rat_pressure_millimeterofmercury = nullptr;
PRAT rat_pressure_pascal = nullptr;
PRAT rat_speed_feetpersecond = nullptr;
PRAT rat_speed_horse = nullptr;
PRAT rat_speed_jet = nullptr;
PRAT rat_speed_kilometersperhour = nullptr;
PRAT rat_speed_knot = nullptr;
PRAT rat_speed_mach = nullptr;
PRAT rat_speed_milesperhour = nullptr;
PRAT rat_speed_turtle = nullptr;
PRAT rat_time_day = nullptr;
PRAT rat_time_hour = nullptr;
PRAT rat_time_week = nullptr;
PRAT rat_time_year = nullptr;
PRAT rat_volume_bathtub = nullptr;
PRAT rat_volume_coffeecup = nullptr;
PRAT rat_volume_cubicfoot = nullptr;
PRAT rat_volume_cubicinch = nullptr;
PRAT rat_volume_cubicyard = nullptr;
PRAT rat_volume_cupus = nullptr;
PRAT rat_volume_fluidounceuk = nullptr;
PRAT rat_volume_fluidounceus = nullptr;
PRAT rat_volume_gallonuk = nullptr;
PRAT rat_volume_gallonus = nullptr;
PRAT rat_volume_pintuk = nullptr;
PRAT rat_volume_pintus = nullptr;
PRAT rat_volume_quartuk = nullptr;
PRAT rat_volume_quartus = nullptr;
PRAT rat_volume_swimmingpool = nullptr;
PRAT rat_volume_tablespoonuk = nullptr;
PRAT rat_volume_tablespoonus = nullptr;
PRAT rat_volume_teaspoonuk = nullptr;
PRAT rat_volume_teaspoonus = nullptr;
PRAT rat_weight_elephant = nullptr;
PRAT rat_weight_longton = nullptr;
PRAT rat_weight_ounce = nullptr;
PRAT rat_weight_pound = nullptr;
PRAT rat_weight_shortton = nullptr;
PRAT rat_weight_soccerball = nullptr;
PRAT rat_weight_stone = nullptr;
PRAT rat_weight_whale = nullptr;

INIT_AND_DUMP_RAW_RAT_FRAQ_IF_NULL(rat, num, div) \
    if (rat == nullptr)            \
    {                              \
        createrat(rat);            \
        DUPNUM(rat->pp, num);      \
        DUPNUM(rat->pq, div);      \
        DUMPRAWRAT(rat);           \
    }

void UnitConverterDataLoader::GetConversionData(_In_ unordered_map<ViewMode, unordered_map<int, PRAT>>& categoryToUnitConversionMap)
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
    INIT_AND_DUMP_RAW_RAT_FRAQ_IF_NULL(rat_data_bit, 1, 8000000);
    INIT_AND_DUMP_RAW_RAT_FRAQ_IF_NULL(rat_length_nanometer, 1, 100000000);
    INIT_AND_DUMP_RAW_RAT_FRAQ_IF_NULL(rat_0_000002, 2, 100000);
    INIT_AND_DUMP_RAW_RAT_FRAQ_IF_NULL(rat_0_000001, 1, 100000);
    INIT_AND_DUMP_RAW_RAT_FRAQ_IF_NULL(rat_0_00001, 1, 10000);
    INIT_AND_DUMP_RAW_RAT_FRAQ_IF_NULL(rat_0_0002, 2, 10000);
    INIT_AND_DUMP_RAW_RAT_FRAQ_IF_NULL(rat_0_0001, 1, 10000);
    INIT_AND_DUMP_RAW_RAT_FRAQ_IF_NULL(rat_0_001, 1, 1000);
    INIT_AND_DUMP_RAW_RAT_FRAQ_IF_NULL(rat_0_01, 1, 100);
    INIT_AND_DUMP_RAW_RAT_FRAQ_IF_NULL(rat_0_1, 1, 10);
    INIT_AND_DUMP_RAW_RAT_FRAQ_IF_NULL(rat_0_9, 9, 10);
INIT_AND_DUMP_RAW_RAT_FRAQ_IF_NULL(rat_angle_gradian, 9, 10);
INIT_AND_DUMP_RAW_RAT_FRAQ_IF_NULL(rat_angle_radian, 5729577951308233, 100000000000000);
INIT_AND_DUMP_RAW_RAT_FRAQ_IF_NULL(rat_area_acre, 40468564224, 10000000);
INIT_AND_DUMP_RAW_RAT_FRAQ_IF_NULL(rat_area_hand, 12516104, 1000000000);
INIT_AND_DUMP_RAW_RAT_FRAQ_IF_NULL(rat_area_paper, 6032246, 100000000);
INIT_AND_DUMP_RAW_RAT_FRAQ_IF_NULL(rat_area_pyeong, 400, 121);
INIT_AND_DUMP_RAW_RAT_FRAQ_IF_NULL(rat_area_soccerfield, 1086966, 100);
INIT_AND_DUMP_RAW_RAT_FRAQ_IF_NULL(rat_area_squarefoot, 144*64516, 10000); 
INIT_AND_DUMP_RAW_RAT_FRAQ_IF_NULL(rat_area_squareinch, 64516, 100000000);
INIT_AND_DUMP_RAW_RAT_FRAQ_IF_NULL(rat_area_squaremile, 4014489600 * 64516, 10000);
INIT_AND_DUMP_RAW_RAT_FRAQ_IF_NULL(rat_area_squareyard, 1296*64516, 10000);
INIT_AND_DUMP_RAW_RAT_IF_NULL(rat_data_cd, 700 * 1024 * 1024);
INIT_AND_DUMP_RAW_RAT_FRAQ_IF_NULL(rat_data_dvd, 47*1024*1024*1024, 10000000); /* precision change */
INIT_AND_DUMP_RAW_RAT_IF_NULL(rat_data_exabits, 125000000000);
INIT_AND_DUMP_RAW_RAT_IF_NULL(rat_data_exabytes, 1000000000000);
INIT_AND_DUMP_RAW_RAT_FRAQ_IF_NULL(rat_data_exbibits, 1024*1024*1024*1024*1024*1024, 8000000);
INIT_AND_DUMP_RAW_RAT_IF_NULL(rat_data_exbibytes, 1024*1024*1024*1024*1024*1024);
INIT_AND_DUMP_RAW_RAT_FRAQ_IF_NULL(rat_data_floppydisk, 144*1024*1024, 100000000); /* precision change */
INIT_AND_DUMP_RAW_RAT_IF_NULL(rat_data_gibibits, 1024*1024*1024, 8000000);
INIT_AND_DUMP_RAW_RAT_IF_NULL(rat_data_gibibytes, 1024*1024*1024);
INIT_AND_DUMP_RAW_RAT_IF_NULL(rat_data_gigabit, 125);
INIT_AND_DUMP_RAW_RAT_FRAQ_IF_NULL(rat_data_kibibits, 1024, 8000000);
INIT_AND_DUMP_RAW_RAT_FRAQ_IF_NULL(rat_data_kibibytes, 1024, 1000000);
INIT_AND_DUMP_RAW_RAT_FRAQ_IF_NULL(rat_data_kilobit, 1, 8000);
INIT_AND_DUMP_RAW_RAT_FRAQ_IF_NULL(rat_data_mebibits, 1024*1024, 8000000);
INIT_AND_DUMP_RAW_RAT_FRAQ_IF_NULL(rat_data_mebibytes, 1024*1024, 1000000);
INIT_AND_DUMP_RAW_RAT_FRAQ_IF_NULL(rat_data_pebibits, 1024*1024*1024*1024*1024, 8000000);
INIT_AND_DUMP_RAW_RAT_FRAQ_IF_NULL(rat_data_pebibytes, 1024*1024*1024*1024*1024, 1000000);
INIT_AND_DUMP_RAW_RAT_IF_NULL(rat_data_petabit, 125000000);
INIT_AND_DUMP_RAW_RAT_IF_NULL(rat_data_petabyte, 1000000000);
INIT_AND_DUMP_RAW_RAT_FRAQ_IF_NULL(rat_data_tebibits, 1024*1024*1024*1024, 8000000);
INIT_AND_DUMP_RAW_RAT_FRAQ_IF_NULL(rat_data_tebibytes, 1024*1024*1024*1024, 1000000);
INIT_AND_DUMP_RAW_RAT_IF_NULL(rat_data_terabit, 125000);
INIT_AND_DUMP_RAW_RAT_FRAQ_IF_NULL(rat_data_yobibits, 1024*1024*1024*1024*1024*1024*1024, 8000000);
INIT_AND_DUMP_RAW_RAT_FRAQ_IF_NULL(rat_data_yobibytes, 1024*1024*1024*1024*1024*1024*1024, 1000000);
INIT_AND_DUMP_RAW_RAT_IF_NULL(rat_data_yottabit, 125000000000000000);
INIT_AND_DUMP_RAW_RAT_IF_NULL(rat_data_yottabyte, 1000000000000000000);
INIT_AND_DUMP_RAW_RAT_FRAQ_IF_NULL(rat_data_zebibits, 1024*1024*1024*1024*1024*1024, 8000000);
INIT_AND_DUMP_RAW_RAT_FRAQ_IF_NULL(rat_data_zebibytes, 1024*1024*1024*1024*1024*1024, 1000000);
INIT_AND_DUMP_RAW_RAT_IF_NULL(rat_data_zetabits, 125000000000000);
INIT_AND_DUMP_RAW_RAT_IF_NULL(rat_data_zetabytes, 1000000000000000);
INIT_AND_DUMP_RAW_RAT_IF_NULL(rat_energy_banana, 439614);
INIT_AND_DUMP_RAW_RAT_IF_NULL(rat_energy_battery, 9000);
INIT_AND_DUMP_RAW_RAT_FRAQ_IF_NULL(rat_energy_britishthermalunit, 10550559, 10000); /* this is a change */
INIT_AND_DUMP_RAW_RAT_FRAQ_IF_NULL(rat_energy_calorie, 4184, 1000); 
INIT_AND_DUMP_RAW_RAT_FRAQ_IF_NULL(rat_energy_electronvolt, 1602176565, 100000000000000000000); /* is this number current/correct? */
INIT_AND_DUMP_RAW_RAT_FRAQ_IF_NULL(rat_energy_footpound, 13558179483314004, 10000000000000000); /* this is a change */
INIT_AND_DUMP_RAW_RAT_IF_NULL(rat_energy_kilocalorie, 4184);
INIT_AND_DUMP_RAW_RAT_IF_NULL(rat_energy_sliceofcake, 1046700);
INIT_AND_DUMP_RAW_RAT_FRAQ_IF_NULL(rat_power_footpoundperminute, 13558179483314004, 60*10000000000000000); /* this is a change */
INIT_AND_DUMP_RAW_RAT_FRAQ_IF_NULL(rat_power_trainengine, 2982799486329081, 1000000000);
INIT_AND_DUMP_RAW_RAT_FRAQ_IF_NULL(rat_speed_feetpersecond, 3048, 100);
INIT_AND_DUMP_RAW_RAT_FRAQ_IF_NULL(rat_speed_horse, 20115, 10);
INIT_AND_DUMP_RAW_RAT_IF_NULL(rat_speed_jet, 24585);
INIT_AND_DUMP_RAW_RAT_FRAQ_IF_NULL(rat_speed_knot, 4 + (51*9), 9); /* this is a change */
INIT_AND_DUMP_RAW_RAT_IF_NULL(rat_speed_mach, 34030);
INIT_AND_DUMP_RAW_RAT_FRAQ_IF_NULL(rat_speed_milesperhour, 447, 10);
INIT_AND_DUMP_RAW_RAT_FRAQ_IF_NULL(rat_speed_turtle, 894, 100);
INIT_AND_DUMP_RAW_RAT_IF_NULL(rat_time_day, 24 * 60 * 60);
INIT_AND_DUMP_RAW_RAT_IF_NULL(rat_time_hour, 60 * 60);
INIT_AND_DUMP_RAW_RAT_IF_NULL(rat_time_week, 7 * 24 * 60 * 60);
INIT_AND_DUMP_RAW_RAT_IF_NULL(rat_time_year, (1461 * 6 /* = 365.25 * 24 */) * 60 * 60);
INIT_AND_DUMP_RAW_RAT_IF_NULL(rat_volume_bathtub, 400 * 946353, 1000);
INIT_AND_DUMP_RAW_RAT_IF_NULL(rat_volume_coffeecup, 2365882, 10000);
INIT_AND_DUMP_RAW_RAT_IF_NULL(rat_volume_cubicfoot, 254*254*254*12*12*12, 100*100*100);
INIT_AND_DUMP_RAW_RAT_IF_NULL(rat_volume_cubicinch, 254*254*254, 100*100*100);
INIT_AND_DUMP_RAW_RAT_IF_NULL(rat_volume_cubicyard, 254*254*254*12*12*12*3*3*3, 100*100*100);
INIT_AND_DUMP_RAW_RAT_IF_NULL(rat_volume_cupus, 236588237, 1000000);
INIT_AND_DUMP_RAW_RAT_IF_NULL(rat_volume_fluidounceuk, 284130625, 10000000);
INIT_AND_DUMP_RAW_RAT_IF_NULL(rat_volume_fluidounceus, 295735295625, 10000000000);
INIT_AND_DUMP_RAW_RAT_IF_NULL(rat_volume_gallonuk, 454609, 100); 
INIT_AND_DUMP_RAW_RAT_IF_NULL(rat_volume_gallonus, 3785411784, 1000000);
INIT_AND_DUMP_RAW_RAT_IF_NULL(rat_volume_pintuk, 56826125, 100000);
INIT_AND_DUMP_RAW_RAT_IF_NULL(rat_volume_pintus, 473176473, 1000000);
INIT_AND_DUMP_RAW_RAT_IF_NULL(rat_volume_quartuk, 11365225, 10000); 
INIT_AND_DUMP_RAW_RAT_IF_NULL(rat_volume_quartus, 946352946, 1000000);
INIT_AND_DUMP_RAW_RAT_IF_NULL(rat_volume_swimmingpool, 3750000000);
INIT_AND_DUMP_RAW_RAT_IF_NULL(rat_volume_tablespoonuk, 177581640625, 10000000000);
INIT_AND_DUMP_RAW_RAT_IF_NULL(rat_volume_tablespoonus, 1478676478125, 100000000000);
INIT_AND_DUMP_RAW_RAT_IF_NULL(rat_volume_teaspoonuk, 1420653125, 240000000); /* this is a change */
INIT_AND_DUMP_RAW_RAT_IF_NULL(rat_volume_teaspoonus, 231*254*254*254, 6*128*100*100*100);
INIT_AND_DUMP_RAW_RAT_IF_NULL(rat_weight_elephant, 4000);
INIT_AND_DUMP_RAW_RAT_IF_NULL(rat_weight_longton, 10160469088, 10000000);
INIT_AND_DUMP_RAW_RAT_IF_NULL(rat_weight_ounce, 45359237, 1600000000);
INIT_AND_DUMP_RAW_RAT_IF_NULL(rat_weight_pound, 45359237, 100000000);
INIT_AND_DUMP_RAW_RAT_IF_NULL(rat_weight_shortton, 90718474, 100000);
INIT_AND_DUMP_RAW_RAT_IF_NULL(rat_weight_soccerball, 4325, 10000);
INIT_AND_DUMP_RAW_RAT_IF_NULL(rat_weight_stone, 14*45359237, 100000000);
INIT_AND_DUMP_RAW_RAT_IF_NULL(rat_weight_whale, 90000);
    /*categoryId, UnitId, factor*/
    static const vector<UnitData> unitDataList = { { ViewMode::Area, UnitConverterUnits::Area_Acre, rat_area_acre },
                                                   { ViewMode::Area, UnitConverterUnits::Area_SquareMeter, rat_one },
                                                   { ViewMode::Area, UnitConverterUnits::Area_SquareFoot, rat_area_squarefoot },
                                                   { ViewMode::Area, UnitConverterUnits::Area_SquareYard, rat_area_squareyard },
                                                   { ViewMode::Area, UnitConverterUnits::Area_SquareMillimeter, rat_0_000001 },
                                                   { ViewMode::Area, UnitConverterUnits::Area_SquareCentimeter, rat_0_0001 },
                                                   { ViewMode::Area, UnitConverterUnits::Area_SquareInch, rat_area_squareinch },
                                                   { ViewMode::Area, UnitConverterUnits::Area_SquareMile, rat_area_squaremile },
                                                   { ViewMode::Area, UnitConverterUnits::Area_SquareKilometer, rat_1000000 },
                                                   { ViewMode::Area, UnitConverterUnits::Area_Hectare, rat_10000 },
                                                   { ViewMode::Area, UnitConverterUnits::Area_Hand, rat_area_hand },
                                                   { ViewMode::Area, UnitConverterUnits::Area_Paper, rat_area_paper },
                                                   { ViewMode::Area, UnitConverterUnits::Area_SoccerField, rat_area_soccerfield },
                                                   { ViewMode::Area, UnitConverterUnits::Area_Castle, rat_100000 },
                                                   { ViewMode::Area, UnitConverterUnits::Area_Pyeong, rat_area_pyeong },

                                                   { ViewMode::Data, UnitConverterUnits::Data_Bit, rat_data_bit },
                                                   { ViewMode::Data, UnitConverterUnits::Data_Byte, rat_0_000001 },
                                                   { ViewMode::Data, UnitConverterUnits::Data_Kilobyte, rat_0_001 },
                                                   { ViewMode::Data, UnitConverterUnits::Data_Megabyte, rat_one },
                                                   { ViewMode::Data, UnitConverterUnits::Data_Gigabyte, rat_1000 },
                                                   { ViewMode::Data, UnitConverterUnits::Data_Terabyte, rat_1000000 },
                                                   { ViewMode::Data, UnitConverterUnits::Data_Petabyte, rat_data_petabyte },
                                                   { ViewMode::Data, UnitConverterUnits::Data_Exabytes, rat_data_exabytes },
                                                   { ViewMode::Data, UnitConverterUnits::Data_Zetabytes, rat_data_zetabytes },
                                                   { ViewMode::Data, UnitConverterUnits::Data_Yottabyte, rat_data_yottabyte },
                                                   { ViewMode::Data, UnitConverterUnits::Data_Kilobit, rat_data_kilobit },
                                                   { ViewMode::Data, UnitConverterUnits::Data_Megabit, rat_0_125 },
                                                   { ViewMode::Data, UnitConverterUnits::Data_Gigabit, rat_data_gigabit },
                                                   { ViewMode::Data, UnitConverterUnits::Data_Terabit, rat_data_terabit },
                                                   { ViewMode::Data, UnitConverterUnits::Data_Petabit, rat_data_petabit },
                                                   { ViewMode::Data, UnitConverterUnits::Data_Exabits, rat_data_exabits },
                                                   { ViewMode::Data, UnitConverterUnits::Data_Zetabits, rat_data_zetabits },
                                                   { ViewMode::Data, UnitConverterUnits::Data_Yottabit, rat_data_yottabit },
                                                   { ViewMode::Data, UnitConverterUnits::Data_Gibibits, rat_data_gibibits },
                                                   { ViewMode::Data, UnitConverterUnits::Data_Gibibytes, rat_data_gibibytes },
                                                   { ViewMode::Data, UnitConverterUnits::Data_Kibibits, rat_data_kibibits },
                                                   { ViewMode::Data, UnitConverterUnits::Data_Kibibytes, rat_data_kibibytes },
                                                   { ViewMode::Data, UnitConverterUnits::Data_Mebibits, rat_data_mebibits },
                                                   { ViewMode::Data, UnitConverterUnits::Data_Mebibytes, rat_data_mebibytes },
                                                   { ViewMode::Data, UnitConverterUnits::Data_Pebibits, rat_data_pebibits },
                                                   { ViewMode::Data, UnitConverterUnits::Data_Pebibytes, rat_data_pebibytes },
                                                   { ViewMode::Data, UnitConverterUnits::Data_Tebibits, rat_data_tebibits },
                                                   { ViewMode::Data, UnitConverterUnits::Data_Tebibytes, rat_data_tebibytes },
                                                   { ViewMode::Data, UnitConverterUnits::Data_Exbibits, rat_data_exbibits },
                                                   { ViewMode::Data, UnitConverterUnits::Data_Exbibytes, rat_data_exbibytes },
                                                   { ViewMode::Data, UnitConverterUnits::Data_Zebibits, rat_data_zebibits },
                                                   { ViewMode::Data, UnitConverterUnits::Data_Zebibytes, rat_data_zebibytes },
                                                   { ViewMode::Data, UnitConverterUnits::Data_Yobibits, rat_data_yobibits },
                                                   { ViewMode::Data, UnitConverterUnits::Data_Yobibytes, rat_data_yobibytes },
                                                   { ViewMode::Data, UnitConverterUnits::Data_FloppyDisk, rat_data_floppydisk },
                                                   { ViewMode::Data, UnitConverterUnits::Data_CD, rat_data_cd },
                                                   { ViewMode::Data, UnitConverterUnits::Data_DVD, rat_data_dvd },

                                                   { ViewMode::Energy, UnitConverterUnits::Energy_Calorie, rat_energy_calorie },
                                                   { ViewMode::Energy, UnitConverterUnits::Energy_Kilocalorie, rat_energy_kilocalorie },
                                                   { ViewMode::Energy, UnitConverterUnits::Energy_BritishThermalUnit, rat_energy_britishthermalunit },
                                                   { ViewMode::Energy, UnitConverterUnits::Energy_Kilojoule, rat_1000 },
                                                   { ViewMode::Energy, UnitConverterUnits::Energy_ElectronVolt, rat_energy_electronvolt },
                                                   { ViewMode::Energy, UnitConverterUnits::Energy_Joule, rat_one },
                                                   { ViewMode::Energy, UnitConverterUnits::Energy_FootPound, rat_energy_footpound },
                                                   { ViewMode::Energy, UnitConverterUnits::Energy_Battery, rat_energy_battery },
                                                   { ViewMode::Energy, UnitConverterUnits::Energy_Banana, rat_energy_banana },
                                                   { ViewMode::Energy, UnitConverterUnits::Energy_SliceOfCake, rat_energy_sliceofcake },

                                                   { ViewMode::Length, UnitConverterUnits::Length_Inch, rat_length_inch },
                                                   { ViewMode::Length, UnitConverterUnits::Length_Foot, rat_foot },
                                                   { ViewMode::Length, UnitConverterUnits::Length_Yard, rat_length_yard },
                                                   { ViewMode::Length, UnitConverterUnits::Length_Mile, rat_length_mile },
                                                   { ViewMode::Length, UnitConverterUnits::Length_Micron, rat_0_000001 },
                                                   { ViewMode::Length, UnitConverterUnits::Length_Millimeter, rat_0_001 },
                                                   { ViewMode::Length, UnitConverterUnits::Length_Nanometer, rat_length_nanometer },
                                                   { ViewMode::Length, UnitConverterUnits::Length_Centimeter, rat_0_01 },
                                                   { ViewMode::Length, UnitConverterUnits::Length_Meter, rat_one },
                                                   { ViewMode::Length, UnitConverterUnits::Length_Kilometer, rat_1000 },
                                                   { ViewMode::Length, UnitConverterUnits::Length_NauticalMile, rat_length_nauticalmile },
                                                   { ViewMode::Length, UnitConverterUnits::Length_Paperclip, rat_length_paperclip },
                                                   { ViewMode::Length, UnitConverterUnits::Length_Hand, rat_length_hand },
                                                   { ViewMode::Length, UnitConverterUnits::Length_JumboJet, rat_length_jumbojet },

                                                   { ViewMode::Power, UnitConverterUnits::Power_BritishThermalUnitPerMinute, rat_power_britishthermalunitperminute },
                                                   { ViewMode::Power, UnitConverterUnits::Power_FootPoundPerMinute, rat_power_footpoundperminute },
                                                   { ViewMode::Power, UnitConverterUnits::Power_Watt, rat_one },
                                                   { ViewMode::Power, UnitConverterUnits::Power_Kilowatt, rat_1000 },
                                                   { ViewMode::Power, UnitConverterUnits::Power_Horsepower, rat_power_horsepower },
                                                   { ViewMode::Power, UnitConverterUnits::Power_LightBulb, rat_60 },
                                                   { ViewMode::Power, UnitConverterUnits::Power_Horse, rat_power_horse },
                                                   { ViewMode::Power, UnitConverterUnits::Power_TrainEngine, rat_power_trainengine },

                                                   { ViewMode::Time, UnitConverterUnits::Time_Day, rat_time_day },
                                                   { ViewMode::Time, UnitConverterUnits::Time_Second, rat_one },
                                                   { ViewMode::Time, UnitConverterUnits::Time_Week, rat_time_week },
                                                   { ViewMode::Time, UnitConverterUnits::Time_Year, rat_time_year },
                                                   { ViewMode::Time, UnitConverterUnits::Time_Millisecond, rat_0_001 },
                                                   { ViewMode::Time, UnitConverterUnits::Time_Microsecond, rat_0_000001 },
                                                   { ViewMode::Time, UnitConverterUnits::Time_Minute, rat_60 },
                                                   { ViewMode::Time, UnitConverterUnits::Time_Hour, rat_time_hour },

                                                   { ViewMode::Volume, UnitConverterUnits::Volume_CupUS, rat_volume_cupus },
                                                   { ViewMode::Volume, UnitConverterUnits::Volume_PintUS, rat_volume_pintus },
                                                   { ViewMode::Volume, UnitConverterUnits::Volume_PintUK, rat_volume_pintuk },
                                                   { ViewMode::Volume, UnitConverterUnits::Volume_QuartUS, rat_volume_quartus },
                                                   { ViewMode::Volume, UnitConverterUnits::Volume_QuartUK, rat_volume_quartuk },
                                                   { ViewMode::Volume, UnitConverterUnits::Volume_GallonUS, rat_volume_gallonus },
                                                   { ViewMode::Volume, UnitConverterUnits::Volume_GallonUK, rat_volume_gallonuk },
                                                   { ViewMode::Volume, UnitConverterUnits::Volume_Liter, rat_1000 },
                                                   { ViewMode::Volume, UnitConverterUnits::Volume_TeaspoonUS, rat_volume_teaspoonus },
                                                   { ViewMode::Volume, UnitConverterUnits::Volume_TablespoonUS, rat_volume_tablespoonus },
                                                   { ViewMode::Volume, UnitConverterUnits::Volume_CubicCentimeter, rat_one },
                                                   { ViewMode::Volume, UnitConverterUnits::Volume_CubicYard, rat_volume_cubicyard },
                                                   { ViewMode::Volume, UnitConverterUnits::Volume_CubicMeter, rat_1000000 },
                                                   { ViewMode::Volume, UnitConverterUnits::Volume_Milliliter, rat_one },
                                                   { ViewMode::Volume, UnitConverterUnits::Volume_CubicInch, rat_volume_cubicinch },
                                                   { ViewMode::Volume, UnitConverterUnits::Volume_CubicFoot, rat_volume_cubicfoot },
                                                   { ViewMode::Volume, UnitConverterUnits::Volume_FluidOunceUS, rat_volume_fluidounceus },
                                                   { ViewMode::Volume, UnitConverterUnits::Volume_FluidOunceUK, rat_volume_fluidounceuk },
                                                   { ViewMode::Volume, UnitConverterUnits::Volume_TeaspoonUK, rat_volume_teaspoonuk },
                                                   { ViewMode::Volume, UnitConverterUnits::Volume_TablespoonUK, rat_volume_tablespoonuk },
                                                   { ViewMode::Volume, UnitConverterUnits::Volume_CoffeeCup, rat_volume_coffeecup },
                                                   { ViewMode::Volume, UnitConverterUnits::Volume_Bathtub, rat_volume_bathtub },
                                                   { ViewMode::Volume, UnitConverterUnits::Volume_SwimmingPool, rat_volume_swimmingpool },

                                                   { ViewMode::Weight, UnitConverterUnits::Weight_Kilogram, rat_one },
                                                   { ViewMode::Weight, UnitConverterUnits::Weight_Hectogram, rat_0_1 },
                                                   { ViewMode::Weight, UnitConverterUnits::Weight_Decagram, rat_0_01 },
                                                   { ViewMode::Weight, UnitConverterUnits::Weight_Gram, rat_0_001 },
                                                   { ViewMode::Weight, UnitConverterUnits::Weight_Pound, rat_weight_pound },
                                                   { ViewMode::Weight, UnitConverterUnits::Weight_Ounce, rat_weight_ounce },
                                                   { ViewMode::Weight, UnitConverterUnits::Weight_Milligram, rat_0_000001 },
                                                   { ViewMode::Weight, UnitConverterUnits::Weight_Centigram, rat_0_00001 },
                                                   { ViewMode::Weight, UnitConverterUnits::Weight_Decigram, rat_0_0001 },
                                                   { ViewMode::Weight, UnitConverterUnits::Weight_LongTon, rat_weight_longton },
                                                   { ViewMode::Weight, UnitConverterUnits::Weight_Tonne, rat_1000 },
                                                   { ViewMode::Weight, UnitConverterUnits::Weight_Stone, rat_weight_stone },
                                                   { ViewMode::Weight, UnitConverterUnits::Weight_Carat, rat_0_0002 },
                                                   { ViewMode::Weight, UnitConverterUnits::Weight_ShortTon, rat_weight_shortton },
                                                   { ViewMode::Weight, UnitConverterUnits::Weight_Snowflake, rat_0_000002 },
                                                   { ViewMode::Weight, UnitConverterUnits::Weight_SoccerBall, rat_weight_soccerball },
                                                   { ViewMode::Weight, UnitConverterUnits::Weight_Elephant, rat_weight_elephant },
                                                   { ViewMode::Weight, UnitConverterUnits::Weight_Whale, rat_weight_whale },

                                                   { ViewMode::Speed, UnitConverterUnits::Speed_CentimetersPerSecond, rat_one },
                                                   { ViewMode::Speed, UnitConverterUnits::Speed_FeetPerSecond, rat_speed_feetpersecond },
                                                   { ViewMode::Speed, UnitConverterUnits::Speed_KilometersPerHour, rat_speed_kilometersperhour },
                                                   { ViewMode::Speed, UnitConverterUnits::Speed_Knot, rat_speed_knot },
                                                   { ViewMode::Speed, UnitConverterUnits::Speed_Mach, rat_speed_mach },
                                                   { ViewMode::Speed, UnitConverterUnits::Speed_MetersPerSecond, rat_100 },
                                                   { ViewMode::Speed, UnitConverterUnits::Speed_MilesPerHour, rat_speed_milesperhour },
                                                   { ViewMode::Speed, UnitConverterUnits::Speed_Turtle, rat_speed_turtle },
                                                   { ViewMode::Speed, UnitConverterUnits::Speed_Horse, rat_speed_horse },
                                                   { ViewMode::Speed, UnitConverterUnits::Speed_Jet, rat_speed_jet },

                                                   { ViewMode::Angle, UnitConverterUnits::Angle_Degree, rat_one },
                                                   { ViewMode::Angle, UnitConverterUnits::Angle_Radian, rat_angle_radian },
                                                   { ViewMode::Angle, UnitConverterUnits::Angle_Gradian, rat_angle_gradian },

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

PRAT rat_temperature_degreescelsius_farenheit = nullptr;
PRAT rat_temperature_degreesfarenheit_celsius = nullptr;
PRAT rat_temperature_offset_celsius_kelvin = nullptr;
PRAT rat_temperature_offset_kelvin_celsius = nullptr;
PRAT rat_temperature_offset_farenheit_celsius = nullptr;
PRAT rat_temperature_offset_celsius_farenheit = nullptr;
PRAT rat_temperature_offset_farenheit_kelvin = nullptr;
PRAT rat_temperature_offset_kelvin_farenheit = nullptr;
void UnitConverterDataLoader::GetExplicitConversionData(_In_ unordered_map<int, unordered_map<int, UCM::ConversionData>>& unitToUnitConversionList)
{
    INIT_AND_DUMP_RAW_RAT_FRAQ_IF_NULL(rat_temperature_degreescelsius_farenheit, 18, 10);
    INIT_AND_DUMP_RAW_RAT_FRAQ_IF_NULL(rat_temperature_degreesfarenheit_celsius, 10, 18);
    INIT_AND_DUMP_RAW_RAT_FRAQ_IF_NULL(rat_temperature_offset_celsius_kelvin, 27315, 100);
    INIT_AND_DUMP_RAW_RAT_FRAQ_IF_NULL(rat_temperature_offset_kelvin_celsius, -27315, 100);
    INIT_AND_DUMP_RAW_RAT_FRAQ_IF_NULL(rat_temperature_offset_farenheit_kelvin, 45967, 100);
    INIT_AND_DUMP_RAW_RAT_FRAQ_IF_NULL(rat_temperature_offset_kelvin_farenheit, -45967, 100);
    INIT_AND_DUMP_RAW_RAT_IF_NULL(rat_temperature_offset_farenheit_celsius, -32);
    INIT_AND_DUMP_RAW_RAT_IF_NULL(rat_temperature_offset_celsius_farenheit, 32);

    /* categoryId, ParentUnitId, UnitId, ratio, offset, offsetfirst*/
    ExplicitUnitConversionData conversionDataList[] = {
        { ViewMode::Temperature, UnitConverterUnits::Temperature_DegreesCelsius, UnitConverterUnits::Temperature_DegreesCelsius, rat_one, rat_zero },
        { ViewMode::Temperature, UnitConverterUnits::Temperature_DegreesCelsius, UnitConverterUnits::Temperature_DegreesFahrenheit, rat_temperature_degreescelsius_farenheit, rat_temperature_offset_celsius_farenheit },
        { ViewMode::Temperature, UnitConverterUnits::Temperature_DegreesCelsius, UnitConverterUnits::Temperature_Kelvin, rat_one, rat_temperature_offset_celsius_kelvin },
        { ViewMode::Temperature,
          UnitConverterUnits::Temperature_DegreesFahrenheit,
          UnitConverterUnits::Temperature_DegreesCelsius,
          rat_temperature_degreesfarenheit_celsius,
          rat_temperature_offset_farenheit_celsius,
          CONVERT_WITH_OFFSET_FIRST },
        { ViewMode::Temperature, UnitConverterUnits::Temperature_DegreesFahrenheit, UnitConverterUnits::Temperature_DegreesFahrenheit, rat_one, rat_zero },
        { ViewMode::Temperature,
          UnitConverterUnits::Temperature_DegreesFahrenheit,
          UnitConverterUnits::Temperature_Kelvin,
          rat_temperature_degreesfarenheit_celsius,
          rat_temperature_offset_farenheit_kelvin,
          CONVERT_WITH_OFFSET_FIRST },
        { ViewMode::Temperature,
          UnitConverterUnits::Temperature_Kelvin,
          UnitConverterUnits::Temperature_DegreesCelsius,
          rat_one,
          rat_temperature_offset_kelvin_celsius,
          CONVERT_WITH_OFFSET_FIRST },
        { ViewMode::Temperature, UnitConverterUnits::Temperature_Kelvin, UnitConverterUnits::Temperature_DegreesFahrenheit, rat_temperature_degreescelsius_farenheit, rat_temperature_offset_kelvin_farenheit },
        { ViewMode::Temperature, UnitConverterUnits::Temperature_Kelvin, UnitConverterUnits::Temperature_Kelvin, rat_one, rat_zero }
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
