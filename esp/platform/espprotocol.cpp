/*##############################################################################

    Copyright (C) 2011 HPCC Systems.

    All rights reserved. This program is free software: you can redistribute it and/or modify
    it under the terms of the GNU Affero General Public License as
    published by the Free Software Foundation, either version 3 of the
    License, or (at your option) any later version.

    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU Affero General Public License for more details.

    You should have received a copy of the GNU Affero General Public License
    along with this program.  If not, see <http://www.gnu.org/licenses/>.
############################################################################## */

#pragma warning (disable : 4786)

#ifdef _WIN32
#ifdef ESPHTTP_EXPORTS
    #define esp_http_decl __declspec(dllexport)
#endif
#endif

#include "platform.h"
#include "espprotocol.hpp"
#include "espbinding.hpp"

typedef IXslProcessor * (*getXslProcessor_func)();


CEspApplicationPort::CEspApplicationPort(bool viewcfg) : bindingCount(0), defBinding(-1), viewConfig(viewcfg), rootAuth(false), navWidth(165), navResize(false), navScroll(false)
{
    build_ver = getBuildVersion();

    hxsl = LoadSharedObject(SharedObjectPrefix "xmllib" SharedObjectExtension, true, false);
    if (!hxsl)
        DBGLOG("Loading xmllib shared library failed!");
    else
    {
        getXslProcessor_func xslfactory = (getXslProcessor_func) GetSharedProcedure(hxsl, "getXslProcessor");
        if (!xslfactory)
            DBGLOG("Loading procedure from xmllib shared library failed!");
        else
            xslp.setown(xslfactory());
    }
}

void CEspApplicationPort::appendBinding(CEspBindingEntry* entry, bool isdefault)
{
    bindings[bindingCount]=entry;
    if (isdefault)
        defBinding=bindingCount;
    bindingCount++;
    EspHttpBinding *httpbind = dynamic_cast<EspHttpBinding *>(entry->queryBinding());
    if (httpbind)
    {
        if (!rootAuth)
            rootAuth = httpbind->rootAuthRequired();
        int width=0;
        bool resizable=false;
        bool scroll=false;
        httpbind->getNavSettings(width, resizable, scroll);
        if (navWidth<width)
            navWidth=width;
        if (!navResize)
            navResize=resizable;
        if (!navScroll)
            navScroll=scroll;
    }
}


const StringBuffer &CEspApplicationPort::getAppFrameHtml(time_t &modified, const char *inner, StringBuffer &html, IEspContext* ctx)
{
    CEspBindingEntry* bindingentry = getDefaultBinding();
    if(bindingentry)
    {
        EspHttpBinding *httpbind = dynamic_cast<EspHttpBinding *>(bindingentry->queryBinding());
        if(httpbind)
        {
            const char* rootpage = httpbind->getRootPage();
            if(rootpage && *rootpage)
            {
                html.loadFile(StringBuffer(getCFD()).append(rootpage).str());
                return html;
            }
        }
    }
    
    if (!xslp)
       throw MakeStringException(0,"Error - CEspApplicationPort XSLT processor not initialized");
    
    bool embedded_url=(inner&&*inner);
    
    StringBuffer params;
    bool needRefresh = true;
    if (!getUrlParams(ctx->queryRequestParameters(), params))
    {
        if (params.length()==0)
            needRefresh = false;
        if (ctx->getClientVersion()>0)
        {
            params.appendf("%cver_=%g", params.length()?'&':'?', ctx->getClientVersion());
            needRefresh = true;
        }
    }

    if (needRefresh || embedded_url || !appFrameHtml.length())
    {
        StringBuffer xml;
        StringBuffer encoded_inner;
        if(inner && *inner)
            encodeXML(inner, encoded_inner);
        
        // replace & with &amps;
        params.replaceString("&","&amp;");

        xml.appendf("<EspApplicationFrame title=\"%s\" navWidth=\"%d\" navResize=\"%d\" navScroll=\"%d\" inner=\"%s\" params=\"%s\"/>", 
            getESPContainer()->getFrameTitle(), navWidth, navResize, navScroll, (inner&&*inner) ? encoded_inner.str() : "?main", params.str());
        Owned<IXslTransform> xform = xslp->createXslTransform();
        xform->setXslSource(StringBuffer(getCFD()).append("./xslt/appframe.xsl").str());
        xform->setXmlSource(xml.str(), xml.length()+1);
        xform->transform( (needRefresh || embedded_url) ? html.clear() : appFrameHtml.clear());
    }

    if (!needRefresh && !embedded_url)
        html.clear().append(appFrameHtml.str());

    static time_t startup_time = time(NULL);    
    modified = startup_time;
    return html;
}
    

const StringBuffer &CEspApplicationPort::getTitleBarHtml(IEspContext& ctx, bool rawXml)
{
    if (xslp)
    {
        StringBuffer titleBarXml;
        const char* user = ctx.queryUserId();
                if (!user || !*user)
            titleBarXml.appendf("<EspHeader><BuildVersion>%s</BuildVersion><ConfigAccess>%d</ConfigAccess>"
                "<LoginId>&lt;nobody&gt;</LoginId><NoUser>1</NoUser></EspHeader>", build_ver, viewConfig);
                else
            titleBarXml.appendf("<EspHeader><BuildVersion>%s</BuildVersion><ConfigAccess>%d</ConfigAccess>"
                "<LoginId>%s</LoginId></EspHeader>", build_ver, viewConfig, user);

        if (rawXml)
        {
            titleBarHtml.set(titleBarXml);
        }
        else
        {
            Owned<IXslTransform> xform = xslp->createXslTransform();
            xform->setXslSource(StringBuffer(getCFD()).append("./xslt/espheader.xsl").str());
            xform->setXmlSource(titleBarXml.str(), titleBarXml.length()+1);
            xform->transform(titleBarHtml.clear());
        }
    }
    return titleBarHtml;
}

const StringBuffer &CEspApplicationPort::getNavBarContent(IEspContext &context, StringBuffer &content, StringBuffer &contentType, bool rawxml)
{
    if (xslp)
    {
        Owned<IPropertyTree> navtree=createPTree("EspNavigationData", false);
        int count = getBindingCount();
        for (int idx = 0; idx<count; idx++)
            bindings[idx]->queryBinding()->getNavigationData(context, *navtree.get());

        StringBuffer xml;
        buildNavTreeXML(navtree.get(), xml);
        if (rawxml)
        {
            content.swapWith(xml);
            contentType.clear().append("text/xml; charset=UTF-8");
        }
        else
        {
            const char* viewType = navtree->queryProp("@viewType");

            Owned<IXslTransform> xform = xslp->createXslTransform();

            StringBuffer xslsource;
            if (viewType && *viewType)
            {
                xslsource.append(getCFD()).appendf("./xslt/%s.xsl", stricmp(viewType, "tree") != 0 ? viewType: "navigation");
            }
            else
            {
                xslsource.append(getCFD()).append("./xslt/nav.xsl");
                    
            }
            xform->setXslSource(xslsource.str());


            xform->setXmlSource(xml.str(), xml.length()+1);
            xform->transform(content);
            contentType.clear().append("text/html; charset=UTF-8");
        }
    }
    return content;
}

const StringBuffer &CEspApplicationPort::getDynNavData(IEspContext &context, IProperties *params, StringBuffer &content, 
                                                       StringBuffer &contentType, bool& bVolatile)
{
    Owned<IPropertyTree> navtree=createPTree("EspDynNavData", false);
    bVolatile = false;
    int count = getBindingCount();
    for (int idx = 0; idx<count; idx++)
        bindings[idx]->queryBinding()->getDynNavData(context, params, *navtree.get());

    if (!bVolatile)
        bVolatile = navtree->getPropBool("@volatile", false);
    contentType.clear().append("text/xml; charset=UTF-8");
    return toXML(navtree.get(), content.clear());
}

int CEspApplicationPort::onGetNavEvent(IEspContext &context, IHttpMessage* request, IHttpMessage* response)
{
    int handled=0;
    int count = getBindingCount();
    for (int idx = 0; !handled && idx<count; idx++)
    {
        handled = bindings[idx]->queryBinding()->onGetNavEvent(context, request, response);
    }
    return handled;
}

int CEspApplicationPort::onBuildSoapRequest(IEspContext &context, IHttpMessage* ireq, IHttpMessage* iresp)
{
    CHttpRequest *request=dynamic_cast<CHttpRequest*>(ireq);
    CHttpResponse *response=dynamic_cast<CHttpResponse*>(iresp);
    
    int handled=0;
    int count = getBindingCount();
    for (int idx = 0; !handled && idx<count; idx++)
    {
        //if (bindings[idx]->queryBinding()->isValidServiceName(context, ))
    }
    return handled;
}

void CEspApplicationPort::buildNavTreeXML(IPropertyTree* navtree, StringBuffer& xmlBuf, bool insideFolder)
{
    if (!navtree)
        return;

    //Find out the menu items which do not request a specific position
    //Also find out the maximum position being requested
    unsigned positionMax = 0;
    StringArray itemsGroup1;
    Owned<IPropertyTreeIterator> items = navtree->getElements("*");
    ForEach(*items)
    {
        IPropertyTree &item = items->query();
        unsigned position = (unsigned) item.getPropInt("@relPosition", 0);
        if (position > positionMax)
        {
            positionMax = position;
        }
        else if (position < 1)
        {//if the item does not request a position, add it to the 'itemsGroup1'.
            StringBuffer itemXML;
            if (!insideFolder)
                buildNavTreeXML(&item, itemXML, true);
            else
                toXML(&item, itemXML);

            itemsGroup1.append(itemXML);
        }
    }

    xmlBuf.appendf("<%s", navtree->queryName());
    Owned<IAttributeIterator> attrs = navtree->getAttributes();
    ForEach(*attrs)
    {
        const char *attrname = attrs->queryName()+1;
        const char *attrvaluee = attrs->queryValue();
        if (attrname && *attrname && attrvaluee && *attrvaluee)
            xmlBuf.appendf(" %s=\"%s\"", attrname, attrvaluee);
    }
    xmlBuf.append(">\n");

    unsigned positionInGroup1 = 0;
    unsigned itemCountInGroup1 = itemsGroup1.length();

    //append the menu items based on the position requested
    unsigned position = 1;
    while (position <= positionMax)
    {
        bool foundOne = false;

        //process the item(s) which asks for this position
        StringBuffer xPath;
        xPath.appendf("*[@relPosition=%d]", position);
        Owned<IPropertyTreeIterator> items1 = navtree->getElements(xPath.str());
        ForEach(*items1)
        {
            IPropertyTree &item = items1->query();

            StringBuffer itemXML;
            if (!insideFolder)
                buildNavTreeXML(&item, itemXML, true);
            else
                toXML(&item, itemXML);
            xmlBuf.append(itemXML.str());

            foundOne = true;
        }

        //If no one asks for this position, pick one from the itemsGroup1
        if (!foundOne && (positionInGroup1 < itemCountInGroup1))
        {
            StringBuffer itemXML = itemsGroup1.item(positionInGroup1);
            xmlBuf.append(itemXML.str());
            positionInGroup1++;
        }

        position++;
    }

    //Check any item left inside the itemsGroup1 and append it into the xml
    while (positionInGroup1 < itemCountInGroup1)
    {
        StringBuffer itemXML = itemsGroup1.item(positionInGroup1);
        xmlBuf.append(itemXML.str());
        positionInGroup1++;
    }

    xmlBuf.appendf("</%s>\n", navtree->queryName());
}

IPropertyTree *CEspBinding::ensureNavFolder(IPropertyTree &root, const char *name, const char *tooltip, const char *menuname, bool sort, unsigned relPosition)
{
    StringBuffer xpath;
    xpath.appendf("Folder[@name=\"%s\"]", name);

    IPropertyTree *ret = root.queryPropTree(xpath.str());
    if (!ret)
    {
        ret=createPTree("Folder", false);
        ret->addProp("@name", name);
        ret->addProp("@tooltip", tooltip);
        if (menuname)
            ret->addProp("@menu", menuname);
        if (sort)
            ret->addPropBool("@sort", true);
        ret->addPropInt("@relPosition", relPosition);

        root.addPropTree("Folder", ret);
    }

    return ret;
}

IPropertyTree *CEspBinding::ensureNavMenu(IPropertyTree &root, const char *name)
{
    StringBuffer xpath;
    xpath.appendf("Menu[@name=\"%s\"]", name);

    IPropertyTree *ret = root.queryPropTree(xpath.str());
    if (!ret)
    {
        ret=createPTree("Menu", false);
        ret->addProp("@name", name);
        root.addPropTree("Menu", ret);
    }
    return ret;
}

IPropertyTree *CEspBinding::ensureNavMenuItem(IPropertyTree &root, const char *name, const char *tooltip, const char *action)
{
    StringBuffer xpath;
    xpath.appendf("MenuItem[@name=\"%s\"]", name);
    IPropertyTree *ret = root.queryPropTree(xpath.str());
    if (!ret)
    {
        ret=createPTree("MenuItem", false);
        ret->addProp("@name", name);
        ret->addProp("@tooltip", tooltip);
        ret->addProp("@action", action);
        root.addPropTree("MenuItem", ret);
    }
    return ret;
}

IPropertyTree *CEspBinding::ensureNavDynFolder(IPropertyTree &root, const char *name, const char *tooltip, const char *params, const char *menuname)
{
    StringBuffer xpath;
    xpath.appendf("DynamicFolder[@name=\"%s\"]", name);

    IPropertyTree *ret = root.queryPropTree(xpath.str());
    if (!ret)
    {
        ret=createPTree("DynamicFolder", false);
        ret->addProp("@name", name);
        ret->addProp("@tooltip", tooltip);
        ret->addProp("@params", params);
        if (menuname)
            ret->addProp("@menu", menuname);
        root.addPropTree("DynamicFolder", ret);
    }
    return ret;
}

IPropertyTree *CEspBinding::ensureNavLink(IPropertyTree &folder, const char *name, const char *path, const char *tooltip, const char *menuname, const char *navPath, unsigned relPosition)
{
    StringBuffer xpath;
    xpath.appendf("Link[@name=\"%s\"]", name);

    IPropertyTree *ret = folder.queryPropTree(xpath.str());
    if (!ret)
    {
        ret=createPTree("Link", false);
        ret->addProp("@name", name);
        if (tooltip)
            ret->addProp("@tooltip", tooltip);
        if (path)
            ret->addProp("@path", path);
        if (menuname)
            ret->addProp("@menu", menuname);
        if (navPath)
            ret->addProp("@navPath", navPath);
        ret->addPropInt("@relPosition", relPosition);

        folder.addPropTree("Link", ret);
    }

    return ret;
}


IPropertyTree *CEspBinding::addNavException(IPropertyTree &folder, const char *message/*=NULL*/, int code/*=0*/, const char *source/*=NULL*/)
{
    IPropertyTree *ret = folder.addPropTree("Exception", createPTree(false));
   ret->addProp("@message", message ? message : "Unknown exception");
    ret->setPropInt("@code", code); 
   if (source)
       ret->addProp("@source", source);
    return ret;
}

void CEspBinding::getNavigationData(IEspContext &context, IPropertyTree & data)
{
    IEspWsdlSections *wsdl = dynamic_cast<IEspWsdlSections *>(this);
    if (wsdl)
    {
        StringBuffer serviceName, params;
        wsdl->getServiceName(serviceName);
        if (!getUrlParams(context.queryRequestParameters(), params))
        {
            if (context.getClientVersion()>0)
                params.appendf("&ver_=%g", context.getClientVersion());
        }
        if (params.length())
            params.setCharAt(0,'&');
        
        IPropertyTree *folder=createPTree("Folder", false);
        folder->addProp("@name", serviceName.str());
        folder->addProp("@info", serviceName.str());
        folder->addProp("@urlParams", params.str());
        if (showSchemaLinks())
            folder->addProp("@showSchemaLinks", "true");
        
        MethodInfoArray methods;
        wsdl->getQualifiedNames(context, methods);
        ForEachItemIn(idx, methods)
        {
            CMethodInfo &method = methods.item(idx);
            IPropertyTree *link=createPTree("Link", false);
            link->addProp("@name", method.m_label.str());
            link->addProp("@info", method.m_label.str());
            StringBuffer path;
            path.appendf("../%s/%s?form%s", serviceName.str(), method.m_label.str(),params.str());
            link->addProp("@path", path.str());

            folder->addPropTree("Link", link);
        }

        data.addPropTree("Folder", folder);
    }
}

void CEspBinding::getDynNavData(IEspContext &context, IProperties *params, IPropertyTree & data)
{
}

CEspProtocol::CEspProtocol() 
{
   m_viewConfig=false;
   m_MaxRequestEntityLength = DEFAULT_MAX_REQUEST_ENTITY_LENGTH;
}

CEspProtocol::~CEspProtocol()
{
    clear();
}

bool CEspProtocol::notifySelected(ISocket *sock,unsigned selected)
{
    return true;
}

const char * CEspProtocol::getProtocolName()
{
    return "ESP Protocol";
}


void CEspProtocol::addBindingMap(ISocket *sock, IEspRpcBinding* binding, bool isdefault)
{
    CEspBindingEntry *entry = new CEspBindingEntry(sock, binding);

    char name[256];
    int port = sock->name(name, 255);

    CApplicationPortMap::iterator apport_it = m_portmap.find(port);

    CEspApplicationPort *apport=NULL;

    if (apport_it!=m_portmap.end())
    {
        apport = (*apport_it).second;
        apport->appendBinding(entry, isdefault);
    }
    else
    {
        apport = new CEspApplicationPort(m_viewConfig);
        apport->appendBinding(entry, isdefault);

        CApplicationPortMap::value_type vt(port, apport);
        m_portmap.insert(vt);
    }
}

CEspApplicationPort* CEspProtocol::queryApplicationPort(int port)
{
    CApplicationPortMap::iterator apport_it = m_portmap.find(port);
    return (apport_it != m_portmap.end()) ? (*apport_it).second : NULL;
}
