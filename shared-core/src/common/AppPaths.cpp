#include <khensu/common/AppPaths.h>
#include <juce_core/juce_core.h>

namespace khensu {

std::string AppPaths::dataDirectory()
{
    auto dir = juce::File::getSpecialLocation(juce::File::userApplicationDataDirectory).getChildFile("KhensuMixing");
    if (!dir.isDirectory()) dir.createDirectory();
    return dir.getFullPathName().toStdString();
}

std::string AppPaths::databaseFile()
{
    return juce::File(juce::String(dataDirectory())).getChildFile("khensu.db").getFullPathName().toStdString();
}

} // namespace khensu
