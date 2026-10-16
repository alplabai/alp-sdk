{ ==========================================================================
  export_e1m_pinmap.pas  --  Altium DelphiScript

  ONE reusable exporter.  Run it on ANY board project (E1M-EVK,
  E1M-X-EVK, a SoM project, ...) to pull GROUND-TRUTH data out of the
  schematics so Alp SDK board metadata can be DERIVED rather than
  hand-transcribed.  One run writes three CSVs next to each other:

    <base>_pinmap.csv      Net, Designator, PinNumber, PinName
    <base>_components.csv  Designator, Comment, DNP, Parameters, UniqueId
    <base>_variants.csv    Variant, Designator, Kind, UniqueId

  WHY pin NAMES matter: a normal netlist export gives net <-> pin NUMBER
  but drops the pin NAME.  For an E1M / E1M-X module connector (e.g. E2)
  the pin name IS the E1M-X pad (IO4, PWM3, ADC0, ENC0_X, ...), so the
  pin-map yields  pad <-> net <-> on-board chip  in one file.  The
  components dump captures every parameter, so the MPN / Manufacturer
  values come straight from the schematic whatever they are named.

  DNP / ASSEMBLY VARIANTS: a board project can carry several assembly
  VARIANTS (Project -> Variants...).  Each variant overrides the base
  design with a list of component VARIATIONS; a component whose variation
  kind is "Not Fitted" is DNP (Do-Not-Populate) in that variant.  The run
  asks WHICH variant to read (the default is the project's current
  variant, else the first one, or the base "[No Variations]" design when
  the project has none) and stamps a DNP column -- 1 for Not-Fitted, else
  0 -- on every component row, so the derived metadata knows what is
  actually populated on that build.  A variant name that does not match
  aborts the run instead of silently exporting everything as fitted.  A
  third file, <base>_variants.csv, lists every variation of every variant
  (Kind is the numeric TVariationKind) so a missing DNP can be traced.

  HOW TO RUN
    1. Open the board project so it is the FOCUSED project.
    2. File -> Run Script... -> pick  ExportAll  -> Run.
    3. At the prompt, set the output base path -- RENAME IT PER BOARD
       (e.g. ...\xevk, ...\e1m_evk, ...\v2n).  Default:
         %TEMP%\board
    4. At the next prompt, pick the assembly variant to read for DNP.
    5. Send all three CSVs over.

  Builds output in memory and writes with SaveToFile (no lingering file
  handle).  The pin-map is written first, so even if the component pass
  hits a DM_* member name your Altium build spells differently, the
  pin-map CSV is already safely on disk.

  API NOTE: components + nets live on the FLATTENED document, not on
  IProject.  If a DM_* member is "Undeclared identifier", paste it --
  likely per-build swaps: DM_DocumentFlattened<->DM_FlattenedDocument,
  DM_NetName<->DM_FlattenedNetName, DM_PhysicalPartDesignator<->
  DM_PartDesignator, DM_ParameterCount/DM_Parameters/IParameter spelling.
  Variant-side swaps to try the same way: DM_ProjectVariantCount/
  DM_ProjectVariants<->DM_VariantCount/DM_Variants, DM_Description<->
  DM_VariantName, DM_VariationCount/DM_Variations<->DM_GraphicalCount/...,
  DM_PhysicalDesignator<->DM_PhysicalPartDesignator on IComponentVariation,
  and the constant eVariation_NotFitted<->eVariationNotFitted,
  DM_CurrentProjectVariant<->DM_CurrentVariant, DM_UniqueId<->DM_UniqueIdPath.
  ========================================================================== }

{ CSV-quote one field: wrap in double quotes and double any embedded quote.
  Needed because parameter values carry inch marks (0.413" Dia) and some
  pin numbers carry commas (a multi-pad pin "1,10"); left raw, either one
  splits the row into extra columns. }
Function Q(S : String) : String;
Var
    i : Integer;
    r : String;
Begin
    r := '';
    For i := 1 To Length(S) Do
    Begin
        If S[i] = '"' Then r := r + '""' Else r := r + S[i];
    End;
    Result := '"' + r + '"';
End;

{ Last segment of a unique-id path ("\ABCDEFGH\IJKLMNOP" -> "IJKLMNOP"). }
Function LastSeg(S : String) : String;
Var
    i : Integer;
Begin
    Result := S;
    For i := Length(S) DownTo 1 Do
        If S[i] = '\' Then Begin Result := Copy(S, i + 1, Length(S) - i); Exit; End;
End;

{ Return the variant named VarName, or Nil when it is blank or not found. }
Function FindVariant(Prj : IProject; VarName : String) : IProjectVariant;
Var
    v : Integer;
Begin
    Result := Nil;
    If VarName = '' Then Exit;
    For v := 0 To Prj.DM_ProjectVariantCount - 1 Do
        If Prj.DM_ProjectVariants(v).DM_Description = VarName Then
        Begin
            Result := Prj.DM_ProjectVariants(v);
            Exit;
        End;
End;

{ Return the DNP (Not-Fitted) set for Variant: one "U:<unique id>" entry per
  Not-Fitted variation.  Altium binds a variation to its part by unique id;
  the designator text stored with it is the one it was created with.  After a
  re-annotation that text can be stale (BT?, R?), and after a part is deleted
  and re-placed under the same designator the old variation stays in the
  project as an orphan whose designator now names a different part.  Matching
  by designator would mark that new part DNP, so only the unique id is used.
  Empty list for Nil (base design, everything fitted).  Caller owns the
  result. }
Function BuildDnpSet(Vnt : IProjectVariant) : TStringList;
Var
    c         : Integer;
    Variation : IComponentVariation;
Begin
    Result := TStringList.Create;
    If Vnt = Nil Then Exit;
    For c := 0 To Vnt.DM_VariationCount - 1 Do
    Begin
        Variation := Vnt.DM_Variations(c);
        If Variation.DM_VariationKind = eVariation_NotFitted Then
        Begin
            Result.Add('U:' + LastSeg(Variation.DM_UniqueId));
        End;
    End;
End;

{ Ask which variant to read.  Lists the available names in the prompt;
  default is the project's CURRENT variant (the one selected in the
  Projects panel), falling back to the first one.  The old default was
  always the first variant, which silently read the wrong DNP set whenever
  the release variant was not first in the list. }
Function PickVariant(Prj : IProject) : String;
Var
    v    : Integer;
    names, dflt : String;
    Cur  : IProjectVariant;
Begin
    names := '';
    dflt  := '';
    For v := 0 To Prj.DM_ProjectVariantCount - 1 Do
    Begin
        names := names + '  ' + Prj.DM_ProjectVariants(v).DM_Description + #13#10;
        If v = 0 Then dflt := Prj.DM_ProjectVariants(v).DM_Description;
    End;
    Cur := Prj.DM_CurrentProjectVariant;
    If Cur <> Nil Then dflt := Cur.DM_Description;
    If names = '' Then names := '  (none -- base design, all fitted)' + #13#10;
    Result := InputBox('ALP board export -- assembly variant',
                       'Variant to read for DNP (blank = base, all fitted):' + #13#10 + names,
                       dflt);
End;

Procedure ExportAll;
Var
    WS      : IWorkspace;
    Prj     : IProject;
    Doc     : IDocument;
    i, k    : Integer;
    Pin     : IPin;
    Comp    : IComponent;
    Param   : IParameter;
    base, params, varName, dnp, tmpDir : String;
    nPins, nComps, nDnp : Integer;
    sl, dnpSet : TStringList;
    Vnt : IProjectVariant;
    Variation : IComponentVariation;
Begin
    WS := GetWorkspace;
    If WS = Nil Then Begin ShowMessage('No workspace open.'); Exit; End;

    Prj := WS.DM_FocusedProject;
    If Prj = Nil Then Begin ShowMessage('Focus a board project first.'); Exit; End;

    Prj.DM_Compile;                       { build connectivity }
    Doc := Prj.DM_DocumentFlattened;      { components + nets live here }
    If Doc = Nil Then Begin ShowMessage('No flattened document.'); Exit; End;

    { Default to the project's own folder -- it always exists and needs no
      %TEMP% expansion (DelphiScript's SaveToFile does NOT expand env vars,
      and this build has no GetEnvironmentVariable).  ExtractFilePath keeps
      the trailing backslash.  (Swap DM_ProjectFullPath<->DM_ProjectFileName
      if "Undeclared".) }
    tmpDir := ExtractFilePath(Prj.DM_ProjectFullPath);
    base := InputBox('ALP board export',
                     'Output base path (no extension) -- rename per board:',
                     tmpDir + 'board');
    If base = '' Then Exit;

    { ---- selected assembly variant -> set of DNP (Not-Fitted) designators ---- }
    varName := PickVariant(Prj);
    Vnt := FindVariant(Prj, varName);
    If (varName <> '') And (Vnt = Nil) Then
    Begin
        ShowMessage('No variant named "' + varName + '" -- nothing exported.');
        Exit;
    End;
    dnpSet := BuildDnpSet(Vnt);

    { ---- 1) pin map : iterate COMPONENTS -> PINS so EVERY pin is listed, ---- }
    { ---- including UNCONNECTED ones (blank Net), e.g. spare bus pads.    ---- }
    sl := TStringList.Create;
    sl.Add('Net,Designator,PinNumber,PinName');
    For i := 0 To Doc.DM_ComponentCount - 1 Do
    Begin
        Comp := Doc.DM_Components(i);
        For k := 0 To Comp.DM_PinCount - 1 Do
        Begin
            Pin := Comp.DM_Pins(k);
            sl.Add(Q(Pin.DM_FlattenedNetName) + ',' +
                   Q(Comp.DM_PhysicalDesignator) + ',' +
                   Q(Pin.DM_PinNumber) + ',' + Q(Pin.DM_PinName));
        End;
    End;
    nPins := sl.Count - 1;
    sl.SaveToFile(base + '_pinmap.csv');
    sl.Free;

    { ---- 2) components : designator, comment, ALL parameters (MPN etc.) ---- }
    sl := TStringList.Create;
    sl.Add('Designator,Comment,DNP,Parameters,UniqueId');
    nDnp := 0;
    For i := 0 To Doc.DM_ComponentCount - 1 Do
    Begin
        Comp := Doc.DM_Components(i);
        params := '';
        For k := 0 To Comp.DM_ParameterCount - 1 Do
        Begin
            Param := Comp.DM_Parameters(k);
            params := params + Param.DM_Name + '=' + Param.DM_Value + ' | ';
        End;
        If dnpSet.IndexOf('U:' + LastSeg(Comp.DM_UniqueId)) >= 0 Then
        Begin
            dnp := '1';
            Inc(nDnp);
        End
        Else
            dnp := '0';
        sl.Add(Q(Comp.DM_PhysicalDesignator) + ',' + Q(Comp.DM_Comment) + ',' + dnp + ',' +
               Q(params) + ',' + Q(Comp.DM_UniqueId));
    End;
    nComps := sl.Count - 1;
    sl.SaveToFile(base + '_components.csv');
    sl.Free;
    dnpSet.Free;

    { ---- 3) variants : every variation of every variant, so a DNP that ---- }
    { ---- did not land in the components file can be traced.           ---- }
    sl := TStringList.Create;
    sl.Add('Variant,Designator,Kind,UniqueId');
    For i := 0 To Prj.DM_ProjectVariantCount - 1 Do
        For k := 0 To Prj.DM_ProjectVariants(i).DM_VariationCount - 1 Do
        Begin
            Variation := Prj.DM_ProjectVariants(i).DM_Variations(k);
            sl.Add(Q(Prj.DM_ProjectVariants(i).DM_Description) + ',' +
                   Q(Variation.DM_PhysicalDesignator) + ',' +
                   IntToStr(Variation.DM_VariationKind) + ',' +
                   Q(Variation.DM_UniqueId));
        End;
    sl.SaveToFile(base + '_variants.csv');
    sl.Free;

    ShowMessage('Done -- ' + IntToStr(nPins) + ' pins, ' + IntToStr(nComps) + ' components, ' +
                IntToStr(nDnp) + ' DNP (variant: ' + varName + '):' + #13#10 +
                base + '_pinmap.csv' + #13#10 + base + '_components.csv' + #13#10 +
                base + '_variants.csv');
End;
